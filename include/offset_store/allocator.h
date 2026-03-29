#ifndef OFFSET_STORE_ALLOCATOR_H
#define OFFSET_STORE_ALLOCATOR_H

#include "offset_store/offset_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file allocator.h
 * @brief Public shared-memory allocator APIs and allocator state snapshots.
 */

/**
 * @brief Public allocator constants.
 */
enum {
    /** Shared allocator layout version stored in private allocator metadata. */
    OFFSET_STORE_ALLOCATOR_VERSION = 2,
    /** Flag indicating that a heap block is currently free. */
    OFFSET_STORE_ALLOCATOR_BLOCK_FREE = 1u
};

/**
 * @brief Snapshot of allocator usage and fragmentation state.
 *
 * The values in this struct are derived from a point-in-time walk of the heap
 * and free blocks. They describe the allocator state visible during the call to
 * `allocator_get_stats(...)`.
 */
typedef struct {
    /** Total heap bytes managed by the allocator. */
    uint64_t heap_size;
    /** Total bytes currently held by free blocks. */
    uint64_t free_bytes;
    /** Total bytes currently consumed by live allocations and block overhead. */
    uint64_t used_bytes;
    /** Size in bytes of the largest free block. */
    uint64_t largest_free_block;
    /** Number of blocks currently on the free list. */
    uint64_t free_block_count;
    /** Cumulative count of allocation attempts that could not be satisfied. */
    uint64_t allocation_failures;
} AllocatorStats;

/**
 * @name Allocator Lifecycle And Validation
 * @{
 */

/**
 * @brief Initializes allocator metadata inside a mapped region.
 *
 * @param region Region whose allocator state should be initialized.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_init(ShmRegion *region);
/**
 * @brief Validates allocator metadata and block structure.
 *
 * @param region Region whose allocator state should be checked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_validate(const ShmRegion *region);

/** @} */

/**
 * @name Allocation Operations
 * @{
 */
/**
 * @brief Allocates a block from the shared heap.
 *
 * @param region Region whose allocator should satisfy the request.
 * @param size Requested payload size in bytes.
 * @param alignment Requested payload alignment in bytes.
 * @param[out] out_ptr Process-local pointer to the allocated payload on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_alloc(ShmRegion *region, size_t size, size_t alignment, void **out_ptr);
/**
 * @brief Frees a previously allocated shared-heap payload.
 *
 * @param region Region whose allocator owns the allocation.
 * @param ptr Process-local pointer previously returned by `allocator_alloc`.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_free(ShmRegion *region, void *ptr);

/** @} */

/**
 * @name Allocation Introspection
 * @{
 */
/**
 * @brief Returns the usable size of a live allocation.
 *
 * @param region Region whose allocator owns the allocation.
 * @param ptr Process-local pointer to the allocation payload.
 * @param[out] out_size Allocation span in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_allocation_span(const ShmRegion *region, const void *ptr, size_t *out_size);
/**
 * @brief Returns the heap start offset recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_heap_offset Heap start offset in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_heap_offset(const ShmRegion *region, uint64_t *out_heap_offset);
/**
 * @brief Returns the total heap size recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_heap_size Heap size in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_heap_size(const ShmRegion *region, uint64_t *out_heap_size);
/**
 * @brief Returns the current free-list head offset.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_head Free-list head on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_free_list_head(const ShmRegion *region, OffsetPtr *out_head);
/**
 * @brief Returns the cumulative allocation-failure count recorded by the allocator.
 *
 * @param region Region descriptor whose allocator should be inspected.
 * @param[out] out_failures Failure count on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_allocation_failures(const ShmRegion *region, uint64_t *out_failures);
/**
 * @brief Returns a snapshot of allocator usage and fragmentation statistics.
 *
 * @param region Region descriptor whose allocator should be inspected.
 * @param[out] out_stats Allocator statistics snapshot on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_get_stats(const ShmRegion *region, AllocatorStats *out_stats);

/** @} */

#endif
