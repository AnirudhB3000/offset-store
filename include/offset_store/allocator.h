#ifndef OFFSET_STORE_ALLOCATOR_H
#define OFFSET_STORE_ALLOCATOR_H

#include "offset_store/offset_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    OFFSET_STORE_ALLOCATOR_VERSION = 1,
    OFFSET_STORE_ALLOCATOR_BLOCK_FREE = 1u
};

OffsetStoreStatus allocator_init(ShmRegion *region);
OffsetStoreStatus allocator_validate(const ShmRegion *region);
OffsetStoreStatus allocator_alloc(ShmRegion *region, size_t size, size_t alignment, void **out_ptr);
OffsetStoreStatus allocator_free(ShmRegion *region, void *ptr);
OffsetStoreStatus allocator_allocation_span(const ShmRegion *region, const void *ptr, size_t *out_size);
OffsetStoreStatus allocator_heap_offset(const ShmRegion *region, uint64_t *out_heap_offset);
OffsetStoreStatus allocator_heap_size(const ShmRegion *region, uint64_t *out_heap_size);
OffsetStoreStatus allocator_free_list_head(const ShmRegion *region, OffsetPtr *out_head);

#endif
