#include "offset_store/allocator.h"

#include <stdint.h>
#include <stdalign.h>

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    uint64_t heap_offset;
    uint64_t heap_size;
    OffsetPtr free_list_head;
} AllocatorHeader;

typedef struct {
    uint64_t size;
    OffsetPtr next_free;
    uint32_t flags;
    uint32_t payload_offset;
} AllocatorBlockHeader;

typedef struct {
    uint64_t block_offset;
} AllocationPrefix;

static const uint64_t OFFSET_STORE_ALLOCATOR_MAGIC = UINT64_C(0x4f464653414c4c43);

static size_t allocator_metadata_offset(void)
{
    /*
     * The allocator begins immediately after the private region header. The
     * region module exposes only the header size so allocator code does not
     * depend on the header typedef itself.
     */
    return shm_region_header_size();
}

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

static AllocatorHeader *allocator_header_mut(ShmRegion *region)
{
    if (region == NULL || region->base == NULL || region->size < allocator_metadata_offset() + sizeof(AllocatorHeader)) {
        return NULL;
    }

    return (AllocatorHeader *) ((unsigned char *) region->base + allocator_metadata_offset());
}

static const AllocatorHeader *allocator_header(const ShmRegion *region)
{
    if (region == NULL || region->base == NULL || region->size < allocator_metadata_offset() + sizeof(AllocatorHeader)) {
        return NULL;
    }

    return (const AllocatorHeader *) ((const unsigned char *) region->base + allocator_metadata_offset());
}

static bool allocator_header_valid(const ShmRegion *region, const AllocatorHeader *header);

OffsetStoreStatus allocator_heap_offset(const ShmRegion *region, uint64_t *out_heap_offset)
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

OffsetStoreStatus allocator_heap_size(const ShmRegion *region, uint64_t *out_heap_size)
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

OffsetStoreStatus allocator_free_list_head(const ShmRegion *region, OffsetPtr *out_head)
{
    const AllocatorHeader *header;

    if (region == NULL || out_head == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = allocator_header(region);
    if (!allocator_header_valid(region, header)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    *out_head = header->free_list_head;
    return OFFSET_STORE_STATUS_OK;
}

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

    if (header->heap_offset >= region->size || header->heap_size > region->size) {
        return false;
    }

    if (header->heap_offset + header->heap_size != region->size) {
        return false;
    }

    return true;
}

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

OffsetStoreStatus allocator_allocation_span(const ShmRegion *region, const void *ptr, size_t *out_size)
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

OffsetStoreStatus allocator_init(ShmRegion *region)
{
    AllocatorHeader *header;
    AllocatorBlockHeader *initial_block;
    OffsetPtr initial_block_offset;
    size_t heap_offset;
    size_t heap_size;

    if (region == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (shm_region_lock(region) != OFFSET_STORE_STATUS_OK) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    if (!allocator_region_offsets(region, &heap_offset, &heap_size) || heap_size < allocator_min_block_size()) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_OUT_OF_MEMORY;
    }

    header = allocator_header_mut(region);
    if (header == NULL) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    if (allocator_header_valid(region, header)) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_ALREADY_EXISTS;
    }

    header->magic = OFFSET_STORE_ALLOCATOR_MAGIC;
    header->version = OFFSET_STORE_ALLOCATOR_VERSION;
    header->reserved = 0;
    header->heap_offset = heap_offset;
    header->heap_size = heap_size;
    header->free_list_head.offset = heap_offset;

    initial_block_offset.offset = heap_offset;
    if (!allocator_block_from_offset(region, initial_block_offset, &initial_block)) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    /*
     * The initial heap is one large free block spanning all usable allocator
     * storage after the fixed metadata area.
     */
    initial_block->size = heap_size;
    initial_block->next_free = offset_ptr_null();
    initial_block->flags = OFFSET_STORE_ALLOCATOR_BLOCK_FREE;
    initial_block->payload_offset = 0;
    if (shm_region_unlock(region) != OFFSET_STORE_STATUS_OK) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }
    return OFFSET_STORE_STATUS_OK;
}

OffsetStoreStatus allocator_validate(const ShmRegion *region)
{
    const AllocatorHeader *header;
    size_t traversed;
    size_t block_count;
    OffsetPtr free_cursor;

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

        traversed += (size_t) block->size;
        block_count += 1;
    }

    free_cursor = header->free_list_head;
    while (!offset_ptr_is_null(free_cursor)) {
        AllocatorBlockHeader *free_block;

        if (block_count == 0 || !allocator_block_is_in_heap(header, free_cursor)) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        if (!allocator_block_from_offset(region, free_cursor, &free_block)) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        if ((free_block->flags & OFFSET_STORE_ALLOCATOR_BLOCK_FREE) == 0) {
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        free_cursor = free_block->next_free;
        block_count -= 1;
    }

    return OFFSET_STORE_STATUS_OK;
}

OffsetStoreStatus allocator_alloc(ShmRegion *region, size_t size, size_t alignment, void **out_ptr)
{
    AllocatorHeader *header;
    OffsetPtr *link;
    OffsetPtr current_offset;

    if (region == NULL || out_ptr == NULL || size == 0) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (alignment < alignof(max_align_t)) {
        alignment = alignof(max_align_t);
    }

    if (!allocator_align_up(size, alignof(max_align_t), &size)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (shm_region_lock(region) != OFFSET_STORE_STATUS_OK) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    header = allocator_header_mut(region);
    if (!allocator_header_valid(region, header)) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    link = &header->free_list_head;
    current_offset = header->free_list_head;
    while (!offset_ptr_is_null(current_offset)) {
        AllocatorBlockHeader *block;
        unsigned char *block_bytes;
        size_t prefix_base;
        size_t payload_offset;
        size_t required_size;
        size_t remaining_size;

        if (!allocator_block_is_in_heap(header, current_offset) ||
            !allocator_block_from_offset(region, current_offset, &block)) {
            shm_region_unlock(region);
            return OFFSET_STORE_STATUS_INVALID_STATE;
        }

        block_bytes = (unsigned char *) block;
        prefix_base = sizeof(AllocatorBlockHeader) + sizeof(AllocationPrefix);
        if (!allocator_align_up(prefix_base + (size_t) current_offset.offset, alignment, &payload_offset)) {
            shm_region_unlock(region);
            return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
        }

        payload_offset -= (size_t) current_offset.offset;
        if (!allocator_align_up(payload_offset + size, alignof(max_align_t), &required_size)) {
            shm_region_unlock(region);
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
                    shm_region_unlock(region);
                    return OFFSET_STORE_STATUS_INVALID_STATE;
                }

                next_block->size = remaining_size;
                next_block->next_free = block->next_free;
                next_block->flags = OFFSET_STORE_ALLOCATOR_BLOCK_FREE;
                next_block->payload_offset = 0;
                *link = next_block_offset;
                block->size = required_size;
            } else {
                *link = block->next_free;
            }

            block->flags = 0;
            block->next_free = offset_ptr_null();
            block->payload_offset = (uint32_t) payload_offset;

            prefix = (AllocationPrefix *) (block_bytes + payload_offset - sizeof(AllocationPrefix));
            prefix->block_offset = current_offset.offset;
            *out_ptr = block_bytes + payload_offset;
            if (shm_region_unlock(region) != OFFSET_STORE_STATUS_OK) {
                return OFFSET_STORE_STATUS_SYSTEM_ERROR;
            }
            return OFFSET_STORE_STATUS_OK;
        }

        link = &block->next_free;
        current_offset = block->next_free;
    }

    shm_region_unlock(region);
    return OFFSET_STORE_STATUS_OUT_OF_MEMORY;
}

OffsetStoreStatus allocator_free(ShmRegion *region, void *ptr)
{
    AllocatorHeader *header;
    AllocationPrefix *prefix;
    OffsetPtr block_offset;
    AllocatorBlockHeader *block;
    unsigned char *expected_payload;

    if (region == NULL || ptr == NULL) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    if (shm_region_lock(region) != OFFSET_STORE_STATUS_OK) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }

    header = allocator_header_mut(region);
    if (!allocator_header_valid(region, header)) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    prefix = (AllocationPrefix *) ((unsigned char *) ptr - sizeof(AllocationPrefix));
    block_offset.offset = prefix->block_offset;
    if (!allocator_block_is_in_heap(header, block_offset) ||
        !allocator_block_from_offset(region, block_offset, &block)) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    if ((block->flags & OFFSET_STORE_ALLOCATOR_BLOCK_FREE) != 0) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    expected_payload = (unsigned char *) block + block->payload_offset;
    if (expected_payload != (unsigned char *) ptr) {
        shm_region_unlock(region);
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    block->flags = OFFSET_STORE_ALLOCATOR_BLOCK_FREE;
    block->payload_offset = 0;
    block->next_free = header->free_list_head;
    header->free_list_head = block_offset;
    if (shm_region_unlock(region) != OFFSET_STORE_STATUS_OK) {
        return OFFSET_STORE_STATUS_SYSTEM_ERROR;
    }
    return OFFSET_STORE_STATUS_OK;
}
