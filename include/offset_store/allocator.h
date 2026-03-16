#ifndef OFFSET_STORE_ALLOCATOR_H
#define OFFSET_STORE_ALLOCATOR_H

#include "offset_store/offset_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Public allocator constants.
 */
enum {
    /** Shared allocator layout version stored in private allocator metadata. */
    OFFSET_STORE_ALLOCATOR_VERSION = 1,
    /** Flag indicating that a heap block is currently free. */
    OFFSET_STORE_ALLOCATOR_BLOCK_FREE = 1u
};

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
/**
 * @brief Returns the usable size of a live allocation.
 *
 * @param region Region whose allocator owns the allocation.
 * @param ptr Process-local pointer to the allocation payload.
 * @param[out] out_size Allocation span in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_allocation_span(const ShmRegion *region, const void *ptr, size_t *out_size);
/**
 * @brief Returns the heap start offset recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_heap_offset Heap start offset in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_heap_offset(const ShmRegion *region, uint64_t *out_heap_offset);
/**
 * @brief Returns the total heap size recorded in allocator metadata.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_heap_size Heap size in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_heap_size(const ShmRegion *region, uint64_t *out_heap_size);
/**
 * @brief Returns the current free-list head offset.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_head Free-list head on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus allocator_free_list_head(const ShmRegion *region, OffsetPtr *out_head);

#endif
