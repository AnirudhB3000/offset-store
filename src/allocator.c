#include "offset_store/allocator.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdalign.h>

// Added includes for deadlock diagnostics
#include <stdio.h>
#include <unistd.h>

// Thread‑local tracking of held shard lock for deadlock detection
static __thread int32_t held_shard = -1;
/* Test hook to set the thread‑local shard tracking variable */
void test_set_held_shard(int32_t val) { held_shard = val; }
/* Test hook to retrieve the current held_shard value */
int32_t test_get_held_shard(void) { return held_shard; }


// Simple diagnostic helper
static void log_deadlock_warning(const char *action, int trying_shard, int current_shard) {
    // Test hook function declared later can call this
    pid_t pid = getpid();
    unsigned long tid = (unsigned long) pthread_self();
    fprintf(stderr, "[deadlock warning] pid=%d tid=%lu action=%s trying_shard=%d while_holding_shard=%d\n",
            pid, tid, action, trying_shard, current_shard);
}


/**
 * @file allocator.c
 * @brief Shared-memory allocator implementation with sharded locking.
 */

/**
 * @name Private Shared-Memory Layout
 * @{
 */

/**
 * @brief Private allocator metadata stored in shared memory.
 *
 * Uses sharded locking: heap is partitioned into N shards, each with its own
 * mutex and free list. This reduces contention under high-load multi-process
 * scenarios by allowing concurrent allocations from different shards.
 */
typedef struct {
    /** Magic value identifying initialized allocator metadata. */
    uint64_t magic;
    /** Allocator layout version. */
    uint32_t version;
    /** Number of shards configured (for validation). */
    uint32_t shard_count;
    /** Offset from the region base to the first heap block. */
    uint64_t heap_offset;
    /** Total heap size in bytes. */
    uint64_t heap_size;
    /** Shard-specific mutexes protecting each free list. */
    pthread_mutex_t shard_mutexes[OFFSET_STORE_ALLOCATOR_SHARD_COUNT];
    /** Shard-specific free-list heads. */
    OffsetPtr shard_free_list_heads[OFFSET_STORE_ALLOCATOR_SHARD_COUNT];
    /** Cumulative count of allocation attempts that could not be satisfied. */
    _Atomic uint64_t allocation_failures;
} AllocatorHeader;

/**
 * @brief Private heap block header stored at the start of every block.
 */
typedef struct {
    /** Total block size, including header and payload area. */
    uint64_t size;
    /** Free-list link used only while the block is free. */
    OffsetPtr next_free;
    /** Block flags such as `OFFSET_STORE_ALLOCATOR_BLOCK_FREE`. */
    uint32_t flags;
    /** Shard index this block belongs to (0 to SHARD_COUNT-1). */
    uint32_t shard;
    /** Byte offset from the block start to the live payload. */
    uint32_t payload_offset;
} AllocatorBlockHeader;

/**
 * @brief In-band prefix stored immediately before every live allocation payload.
 */
typedef struct {
    /** Offset of the owning heap block from the region base. */
    uint64_t block_offset;
} AllocationPrefix;

/** @} */

static const uint64_t OFFSET_STORE_ALLOCATOR_MAGIC = UINT64_C(0x4f464653414c4c43);

#if defined(PTHREAD_MUTEX_ROBUST)
#define OFFSET_STORE_PTHREAD_MUTEX_ROBUST_VALUE PTHREAD_MUTEX_ROBUST
#elif defined(PTHREAD_MUTEX_ROBUST_NP)
#define OFFSET_STORE_PTHREAD_MUTEX_ROBUST_VALUE PTHREAD_MUTEX_ROBUST_NP
#endif

/**
 * @name Internal Helpers
 * @{
 */

/**
 * @brief Returns the offset where private allocator metadata begins.
 *
 * @return Byte offset from the region base.
 */
static size_t allocator_metadata_offset(void)
{
    /*
     * The allocator begins immediately after the private region header. The
     * region module exposes only the header size so allocator code does not
     * depend on the header typedef itself.
     */
    return shm_region_header_size();
}

/**
 * @brief Rounds a value up to the requested power-of-two alignment.
 *
 * @param value Input value to align.
 * @param alignment Requested alignment in bytes.
 * @param[out] out_value Aligned value on success.
 * @return true if alignment succeeds without overflow.
 * @return false otherwise.
 */
static bool allocator_align_up(size_t value, size_t alignment, size_t *out_value)
{
    size_t aligned;

    if (out_value == NULL || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return false;
    }

    if (value > SIZE_MAX - (alignment - 1)) {
        return false;
    }

    aligned = (value + (alignment - 1)) & ~(alignment - 1);
    *out_value = aligned;
    return true;
}

/**
 * @brief Returns the minimum size required for a valid heap block.
 *
 * @return Minimum block size in bytes.
 */
static size_t allocator_min_block_size(void)
{
    size_t min_block_size;

    /*
     * Any remainder block must be able to hold another block header plus the
     * prefix and at least one aligned byte of payload for a future allocation.
     */
    min_block_size = sizeof(AllocatorBlockHeader) + sizeof(AllocationPrefix) + alignof(max_align_t);
    return min_block_size;
}

/**
 * @brief Selects a shard index for a given allocation request.
 *
 * Uses size-based routing: larger allocations tend to use higher-indexed shards
 * to improve locality. Combines with a simple hash of the request size to
 * distribute similar-sized allocations across shards.
 *
 * @param size Requested allocation size in bytes.
 * @return Shard index in range [0, SHARD_COUNT).
 */
static uint32_t allocator_select_shard(size_t size)
{
    uint32_t shard;
    size_t hash;

    hash = size ^ (size >> 17);
    shard = (uint32_t)(hash & (OFFSET_STORE_ALLOCATOR_SHARD_COUNT - 1));
    return shard;
}

/**
 * @brief Returns the heap offset where a given shard begins.
 *
 * @param heap_offset Offset where overall heap starts.
 * @param heap_size Total heap size in bytes.
 * @param shard Index of the shard.
 * @return Byte offset from region base where shard begins.
 */
static uint64_t allocator_shard_heap_offset(uint64_t heap_offset, uint64_t heap_size, uint32_t shard)
{
    uint64_t shard_size;
    uint64_t shard_offset;

    shard_size = heap_size / OFFSET_STORE_ALLOCATOR_SHARD_COUNT;
    shard_offset = heap_offset + (shard * shard_size);
    return shard_offset;
}

/**
 * @brief Returns the size of a single shard's heap region.
 *
 * @param heap_size Total heap size in bytes.
 * @return Size of one shard's heap region in bytes.
 */
static uint64_t allocator_shard_heap_size(uint64_t heap_size)
{
    return heap_size / OFFSET_STORE_ALLOCATOR_SHARD_COUNT;
}

/**
 * @brief Computes the aligned heap offset and size for a region.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_heap_offset Heap start offset on success.
 * @param[out] out_heap_size Heap size on success.
 * @return true if the region can host allocator metadata and heap storage.
 * @return false otherwise.
 */
static bool allocator_region_offsets(const ShmRegion *region, size_t *out_heap_offset, size_t *out_heap_size)
{
    size_t heap_start;
    size_t aligned_heap_start;

    if (region == NULL || out_heap_offset == NULL || out_heap_size == NULL) {
        return false;
    }

    if (region->size < allocator_metadata_offset() + sizeof(AllocatorHeader)) {
        return false;
    }

    heap_start = allocator_metadata_offset() + sizeof(AllocatorHeader);
    if (!allocator_align_up(heap_start, alignof(max_align_t), &aligned_heap_start)) {
        return false;
    }

    if (aligned_heap_start >= region->size) {
        return false;
    }

    *out_heap_offset = aligned_heap_start;
    *out_heap_size = region->size - aligned_heap_start;
    return true;
}

/**
 * @brief Returns a mutable pointer to the private allocator header.
 *
 * @param region Region descriptor to inspect.
 * @return Mutable allocator header pointer, or `NULL` on failure.
 */
static AllocatorHeader *allocator_header_mut(ShmRegion *region)
{
    if (region == NULL || region->base == NULL || region->size < allocator_metadata_offset() + sizeof(AllocatorHeader)) {
        return NULL;
    }

    return (AllocatorHeader *) ((unsigned char *) region->base + allocator_metadata_offset());
}

/**
 * @brief Returns a const pointer to the private allocator header.
 *
 * @param region Region descriptor to inspect.
 * @return Const allocator header pointer, or `NULL` on failure.
 */
static const AllocatorHeader *allocator_header(const ShmRegion *region)
{
    if (region == NULL || region->base == NULL || region->size < allocator_metadata_offset() + sizeof(AllocatorHeader)) {
        return NULL;
    }

    return (const AllocatorHeader *) ((const unsigned char *) region->base + allocator_metadata_offset());
}

/**
 * @brief Returns whether the private allocator header is structurally valid.
 *
 * @param region Region descriptor to inspect.
 * @param header Candidate allocator header.
 * @return true if the header is valid.
 * @return false otherwise.
 */
static bool allocator_header_valid(const ShmRegion *region, const AllocatorHeader *header);

/** @} */

/**
 * @name Public Introspection
 * @{
 */

/**
 * @brief Returns the heap start offset recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_heap_offset Heap start offset on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_heap_offset(const ShmRegion *region, uint64_t *out_heap_offset)
{
    const AllocatorHeader *header;

    if (region == NULL || out_heap_offset == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    *out_heap_offset = header->heap_offset;
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Returns the heap size recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_heap_size Heap size on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_heap_size(const ShmRegion *region, uint64_t *out_heap_size)
{
    const AllocatorHeader *header;

    if (region == NULL || out_heap_size == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    *out_heap_size = header->heap_size;
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Returns the current free-list head recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_head Free-list head on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_free_list_head(const ShmRegion *region, OffsetPtr *out_head)
{
    const AllocatorHeader *header;

    if (region == NULL || out_head == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    *out_head = header->shard_free_list_heads[0];
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Returns the cumulative allocation-failure count recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_failures Failure count on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_allocation_failures(const ShmRegion *region, uint64_t *out_failures)
{
    const AllocatorHeader *header;

    if (region == NULL || out_failures == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    *out_failures = header->allocation_failures;
    return OFFSET_STORE_STATUS_OK;
}

/** @} */

/**
 * @name Internal Heap Traversal Helpers
 * @{
 */

static bool allocator_header_valid(const ShmRegion *region, const AllocatorHeader *header)
{
    if (region == NULL || header == NULL) {
        return false;
    }

    if (header->magic != OFFSET_STORE_ALLOCATOR_MAGIC) {
        return false;
    }

    if (header->version != OFFSET_STORE_ALLOCATOR_VERSION) {
        return false;
    }

    if (header->shard_count != OFFSET_STORE_ALLOCATOR_SHARD_COUNT) {
        return false;
    }

    if (header->heap_offset >= region->size || header->heap_size > region->size) {
        return false;
    }

    if (header->heap_offset + header->heap_size != region->size) {
        return false;
    }

    return true;
}

/**
 * @brief Resolves a heap block offset to a mutable block header pointer.
 *
 * @param region Region whose mapping contains the block.
 * @param block_offset Offset of the block to resolve.
 * @param[out] out_block Resolved block pointer on success.
 * @return true if resolution succeeds.
 * @return false otherwise.
 */
static bool allocator_block_from_offset(
    const ShmRegion *region,
    OffsetPtr block_offset,
    AllocatorBlockHeader **out_block
)
{
    void *resolved;

    if (out_block == NULL) {
        return false;
    }

    if (!offset_ptr_try_resolve(region->base, region->size, block_offset, sizeof(AllocatorBlockHeader), &resolved)) {
        return false;
    }

    *out_block = (AllocatorBlockHeader *) resolved;
    return true;
}

/**
 * @brief Returns whether a block offset lies within the allocator heap range.
 *
 * @param header Allocator header describing the heap bounds.
 * @param block_offset Offset to inspect.
 * @return true if the offset lies within the heap.
 * @return false otherwise.
 */
#if defined(__GNUC__)
__attribute__((unused))
#endif
static bool allocator_block_is_in_heap(const AllocatorHeader *header, OffsetPtr block_offset)
{
    if (header == NULL || offset_ptr_is_null(block_offset)) {
        return false;
    }

    if (block_offset.offset < header->heap_offset) {
        return false;
    }

    if (block_offset.offset >= header->heap_offset + header->heap_size) {
        return false;
    }

    return true;
}

/** @} */

/**
 * @name Public Statistics
 * @{
 */

/**
 * @brief Returns a snapshot of allocator usage and free-space statistics.
 *
 * @param region Region descriptor whose allocator should be inspected.
 * @param[out] out_stats Statistics snapshot on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_stats(const ShmRegion *region, AllocatorStats *out_stats)
{
    const AllocatorHeader *header;
    size_t traversed;

    if (region == NULL || out_stats == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    out_stats->heap_size = header->heap_size;
    out_stats->free_bytes = 0;
    out_stats->used_bytes = 0;
    out_stats->largest_free_block = 0;
    out_stats->free_block_count = 0;
    out_stats->allocation_failures = header->allocation_failures;

    traversed = 0;
    while (traversed < header->heap_size) {
        OffsetPtr block_offset;
        AllocatorBlockHeader *block;
        uint64_t block_size;

        block_offset.offset = header->heap_offset + traversed;
        if (!allocator_block_from_offset(region, block_offset, &block)) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        block_size = block->size;
        if (block_size == 0 || block_size > header->heap_size - traversed) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        if ((block->flags & OFFSET_STORE_ALLOCATOR_BLOCK_FREE) != 0u) {
            out_stats->free_bytes += block_size;
            out_stats->free_block_count += 1;
            if (block_size > out_stats->largest_free_block) {
                out_stats->largest_free_block = block_size;
            }
        } else {
            out_stats->used_bytes += block_size;
        }

        traversed += (size_t) block_size;
    }

    return OFFSET_STORE_STATUS_OK;
}

/** @} */

/**
 * @brief Returns the usable span of a live allocation.
 *
 * @param region Region whose allocator owns the allocation.
 * @param ptr Process-local payload pointer.
 * @param[out] out_size Allocation span on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_allocation_span(const ShmRegion *region, const void *ptr, size_t *out_size)
{
    const AllocatorHeader *header;
    size_t traversed;

    if (region == NULL || ptr == NULL || out_size == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    traversed = 0;
    while (traversed < header->heap_size) {
        OffsetPtr block_offset;
        AllocatorBlockHeader *block;
        unsigned char *allocation_start;

        block_offset.offset = header->heap_offset + traversed;
        if (!allocator_block_from_offset(region, block_offset, &block)) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        if ((block->flags & OFFSET_STORE_ALLOCATOR_BLOCK_FREE) == 0) {
            allocation_start = (unsigned char *) block + block->payload_offset;
            if (allocation_start == (const unsigned char *) ptr) {
                *out_size = (size_t) block->size - block->payload_offset;
                return OFFSET_STORE_STATUS_OK;
            }
        }

        traversed += (size_t) block->size;
    }

    return OFFSET_STORE_STATUS_NOT_FOUND;
}

/**
 * @brief Initializes allocator metadata and seeds the initial free block.
 *
 * @param region Region whose allocator state should be initialized.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_init(ShmRegion *region)
{
    AllocatorHeader *header;
    size_t heap_offset;
    size_t heap_size;
    uint32_t shard;
    uint32_t i;
    pthread_mutexattr_t mutex_attr;

    if (region == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (shm_region_allocator_lock(region) != OFFSET_STORE_STATUS_OK) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (!allocator_region_offsets(region, &heap_offset, &heap_size) ||
        heap_size < allocator_min_block_size() * OFFSET_STORE_ALLOCATOR_SHARD_COUNT) {
        shm_region_allocator_unlock(region);
        return OFFSET_STORE_STATUS_OUT_OF_MEMORY;
    }

    header = allocator_header_mut(region);
    if (header == NULL) {
        shm_region_allocator_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    if (allocator_header_valid(region, header)) {
        shm_region_allocator_unlock(region);
        return OFFSET_STORE_STATUS_ALREADY_EXISTS;
    }

    if (pthread_mutexattr_init(&mutex_attr) != 0) {
        shm_region_allocator_unlock(region);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&mutex_attr);
        shm_region_allocator_unlock(region);
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

#if defined(OFFSET_STORE_PTHREAD_MUTEX_ROBUST_VALUE)
    (void) pthread_mutexattr_setrobust(&mutex_attr, OFFSET_STORE_PTHREAD_MUTEX_ROBUST_VALUE);
#endif

    header->magic = OFFSET_STORE_ALLOCATOR_MAGIC;
    header->version = OFFSET_STORE_ALLOCATOR_VERSION;
    header->shard_count = OFFSET_STORE_ALLOCATOR_SHARD_COUNT;
    header->heap_offset = heap_offset;
    header->heap_size = heap_size;
    header->allocation_failures = 0;

    for (shard = 0; shard < OFFSET_STORE_ALLOCATOR_SHARD_COUNT; shard++) {
        if (pthread_mutex_init(&header->shard_mutexes[shard], &mutex_attr) != 0) {
            for (i = 0; i < shard; i++) {
                (void) pthread_mutex_destroy(&header->shard_mutexes[i]);
            }
            pthread_mutexattr_destroy(&mutex_attr);
            shm_region_allocator_unlock(region);
            return OFFSET_STORE_STATUS_SYSTEM_ERROR;
        }
    }

    pthread_mutexattr_destroy(&mutex_attr);

    for (shard = 0; shard < OFFSET_STORE_ALLOCATOR_SHARD_COUNT; shard++) {
        uint64_t shard_heap_offset;
        uint64_t shard_heap_size;
        AllocatorBlockHeader *shard_initial_block;
        OffsetPtr shard_initial_offset;

        shard_heap_offset = allocator_shard_heap_offset(heap_offset, heap_size, shard);
        shard_heap_size = allocator_shard_heap_size(heap_size);

        header->shard_free_list_heads[shard].offset = shard_heap_offset;

        shard_initial_offset.offset = shard_heap_offset;
        if (!allocator_block_from_offset(region, shard_initial_offset, &shard_initial_block)) {
            for (i = 0; i < OFFSET_STORE_ALLOCATOR_SHARD_COUNT; i++) {
                (void) pthread_mutex_destroy(&header->shard_mutexes[i]);
            }
            shm_region_allocator_unlock(region);
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        shard_initial_block->size = shard_heap_size;
        shard_initial_block->next_free = offset_ptr_null();
        shard_initial_block->flags = OFFSET_STORE_ALLOCATOR_BLOCK_FREE;
        shard_initial_block->shard = shard;
        shard_initial_block->payload_offset = 0;
    }

    if (shm_region_allocator_unlock(region) != OFFSET_STORE_STATUS_OK) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }
    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Validates physical heap structure and free-list integrity.
 *
 * @param region Region whose allocator state should be checked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_validate(const ShmRegion *region)
{
    const AllocatorHeader *header;
    size_t traversed;
    size_t block_count;
    uint32_t shard;

    if (region == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    traversed = 0;
    block_count = 0;
    while (traversed < header->heap_size) {
        OffsetPtr block_offset;
        AllocatorBlockHeader *block;

        block_offset.offset = header->heap_offset + traversed;
        if (!allocator_block_from_offset(region, block_offset, &block)) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        if (block->size < sizeof(AllocatorBlockHeader) || block->size > header->heap_size - traversed) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        if (block->flags != 0 && block->flags != OFFSET_STORE_ALLOCATOR_BLOCK_FREE) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        if (block->shard >= OFFSET_STORE_ALLOCATOR_SHARD_COUNT) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        traversed += (size_t) block->size;
        block_count += 1;
    }

    for (shard = 0; shard < OFFSET_STORE_ALLOCATOR_SHARD_COUNT; shard++) {
        OffsetPtr free_cursor;
        uint64_t shard_heap_offset;
        uint64_t shard_heap_size;

        shard_heap_offset = allocator_shard_heap_offset(header->heap_offset, header->heap_size, shard);
        shard_heap_size = allocator_shard_heap_size(header->heap_size);

        free_cursor = header->shard_free_list_heads[shard];
        while (!offset_ptr_is_null(free_cursor)) {
            AllocatorBlockHeader *free_block;

            if (block_count == 0) {
                return OFFSET_STORE_STATUS_INVALID_STATE;
            }

            if (free_cursor.offset < shard_heap_offset ||
                free_cursor.offset >= shard_heap_offset + shard_heap_size) {
                return OFFSET_STORE_STATUS_INVALID_STATE;
            }

            if (!allocator_block_from_offset(region, free_cursor, &free_block)) {
                return OFFSET_STORE_STATUS_INVALID_STATE;
            }

            if ((free_block->flags & OFFSET_STORE_ALLOCATOR_BLOCK_FREE) == 0) {
                return OFFSET_STORE_STATUS_INVALID_STATE;
            }

            if (free_block->shard != shard) {
                return OFFSET_STORE_STATUS_INVALID_STATE;
            }

            free_cursor = free_block->next_free;
            block_count -= 1;
        }
    }

    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Allocates a block from the shared heap.
 *
 * @param region Region whose allocator should satisfy the request.
 * @param size Requested payload size in bytes.
 * @param alignment Requested payload alignment in bytes.
 * @param[out] out_ptr Process-local payload pointer on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_alloc(ShmRegion *region, size_t size, size_t alignment, void **out_ptr)
{
    AllocatorHeader *header;
    uint32_t shard;
    uint32_t attempt;
    int lock_result;

    if (region == NULL || out_ptr == NULL || size == 0) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (alignment < alignof(max_align_t)) {
        alignment = alignof(max_align_t);
    }

    if (!allocator_align_up(size, alignof(max_align_t), &size)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header_mut(region);
    if (header == NULL || !allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    shard = allocator_select_shard(size);

    for (attempt = 0; attempt < OFFSET_STORE_ALLOCATOR_SHARD_COUNT; attempt++) {
        uint32_t try_shard = (shard + attempt) % OFFSET_STORE_ALLOCATOR_SHARD_COUNT;
        OffsetPtr *link;
        OffsetPtr current_offset;
        pthread_mutex_t *shard_mutex;

        shard_mutex = &header->shard_mutexes[try_shard];
        lock_result = pthread_mutex_lock(shard_mutex);
        // Deadlock detection: if this thread already holds a different shard lock, warn
        if (held_shard != -1 && (uint32_t)held_shard != try_shard) {
            log_deadlock_warning("allocator_alloc_lock", try_shard, held_shard);
        }
        // Record that we now hold this shard
        held_shard = try_shard;
#if defined(EOWNERDEAD)
        if (lock_result == EOWNERDEAD) {
            (void) pthread_mutex_consistent(shard_mutex);
        }
#endif
        if (lock_result != 0) {
            return OFFSET_STORE_STATUS_SYSTEM_ERROR;
        }

        if (!allocator_header_valid(region, header)) {
            (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        link = &header->shard_free_list_heads[try_shard];
        current_offset = *link;

        while (!offset_ptr_is_null(current_offset)) {
            AllocatorBlockHeader *block;
            unsigned char *block_bytes;
            size_t prefix_base;
            size_t payload_offset;
            size_t required_size;
            size_t remaining_size;
            uint64_t shard_heap_offset;
            uint64_t shard_heap_size;

            shard_heap_offset = allocator_shard_heap_offset(header->heap_offset, header->heap_size, try_shard);
            shard_heap_size = allocator_shard_heap_size(header->heap_size);

            if (current_offset.offset < shard_heap_offset ||
                current_offset.offset >= shard_heap_offset + shard_heap_size) {
                (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
                return OFFSET_STORE_STATUS_INVALID_STATE;
            }

            if (!allocator_block_from_offset(region, current_offset, &block)) {
                (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
                return OFFSET_STORE_STATUS_INVALID_STATE;
            }

            block_bytes = (unsigned char *) block;
            prefix_base = sizeof(AllocatorBlockHeader) + sizeof(AllocationPrefix);
            if (!allocator_align_up(prefix_base + (size_t) current_offset.offset, alignment, &payload_offset)) {
                (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
                return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
            }

            payload_offset -= (size_t) current_offset.offset;
            if (!allocator_align_up(payload_offset + size, alignof(max_align_t), &required_size)) {
                (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
                return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
            }

            if (required_size <= block->size) {
                AllocationPrefix *prefix;

                remaining_size = (size_t) block->size - required_size;
                if (remaining_size >= allocator_min_block_size()) {
                    AllocatorBlockHeader *next_block;
                    OffsetPtr next_block_offset;

                    next_block_offset.offset = current_offset.offset + required_size;
                    if (!allocator_block_from_offset(region, next_block_offset, &next_block)) {
                        (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
                        return OFFSET_STORE_STATUS_INVALID_STATE;
                    }

                    next_block->size = remaining_size;
                    next_block->next_free = block->next_free;
                    next_block->flags = OFFSET_STORE_ALLOCATOR_BLOCK_FREE;
                    next_block->shard = try_shard;
                    next_block->payload_offset = 0;
                    *link = next_block_offset;
                    block->size = required_size;
                } else {
                    *link = block->next_free;
                }

                block->flags = 0;
                block->next_free = offset_ptr_null();
                block->shard = try_shard;
                block->payload_offset = (uint32_t) payload_offset;

                prefix = (AllocationPrefix *) (block_bytes + payload_offset - sizeof(AllocationPrefix));
                prefix->block_offset = current_offset.offset;
                *out_ptr = block_bytes + payload_offset;
                (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
                return OFFSET_STORE_STATUS_OK;
            }

            link = &block->next_free;
            current_offset = block->next_free;
        }

        (void) pthread_mutex_unlock(shard_mutex);
                // Clear held shard tracking after unlocking
                held_shard = -1;
    }

    (void) atomic_fetch_add_explicit(&header->allocation_failures, 1, memory_order_relaxed);
    return OFFSET_STORE_STATUS_OUT_OF_MEMORY;
}

/**
 * @brief Frees a payload previously allocated from the shared heap.
 *
 * @param region Region whose allocator owns the allocation.
 * @param ptr Process-local payload pointer to free.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_free(ShmRegion *region, void *ptr)
{
    AllocatorHeader *header;
    AllocationPrefix *prefix;
    OffsetPtr block_offset;
    AllocatorBlockHeader *block;
    unsigned char *expected_payload;
    uint32_t shard;
    int lock_result;

    if (region == NULL || ptr == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header_mut(region);
    if (header == NULL || !allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    prefix = (AllocationPrefix *) ((unsigned char *) ptr - sizeof(AllocationPrefix));
    block_offset.offset = prefix->block_offset;

    if (!allocator_block_from_offset(region, block_offset, &block)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    if ((block->flags & OFFSET_STORE_ALLOCATOR_BLOCK_FREE) != 0) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    shard = block->shard;
    if (shard >= OFFSET_STORE_ALLOCATOR_SHARD_COUNT) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    lock_result = pthread_mutex_lock(&header->shard_mutexes[shard]);
        // Deadlock detection: if this thread already holds a different shard lock, warn
        if (held_shard != -1 && (uint32_t)held_shard != shard) {
            log_deadlock_warning("allocator_free_lock", shard, held_shard);
        }
        // Record that we now hold this shard
        held_shard = shard;
#if defined(EOWNERDEAD)
    if (lock_result == EOWNERDEAD) {
        (void) pthread_mutex_consistent(&header->shard_mutexes[shard]);
    }
#endif
    if (lock_result != 0) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (!allocator_header_valid(region, header)) {
        (void) pthread_mutex_unlock(&header->shard_mutexes[shard]);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    if (!allocator_block_from_offset(region, block_offset, &block)) {
        (void) pthread_mutex_unlock(&header->shard_mutexes[shard]);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    expected_payload = (unsigned char *) block + block->payload_offset;
    if (expected_payload != (unsigned char *) ptr) {
        (void) pthread_mutex_unlock(&header->shard_mutexes[shard]);
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    block->flags = OFFSET_STORE_ALLOCATOR_BLOCK_FREE;
    block->payload_offset = 0;
    block->next_free = header->shard_free_list_heads[shard];
    header->shard_free_list_heads[shard] = block_offset;
    (void) pthread_mutex_unlock(&header->shard_mutexes[shard]);
    // Clear held shard tracking after unlocking
    held_shard = -1;
    return OFFSET_STORE_STATUS_OK;
}
