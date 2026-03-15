#ifndef OFFSET_STORE_ALLOCATOR_H
#define OFFSET_STORE_ALLOCATOR_H

#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The allocator header is stored inside the shared region immediately after the
 * region header. It describes where the heap starts and which free block is at
 * the head of the free list.
 */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    uint64_t heap_offset;
    uint64_t heap_size;
    OffsetPtr free_list_head;
} AllocatorHeader;

/*
 * Every heap block starts with this header. next_free is meaningful only when
 * the block is free; allocated blocks still keep the field for stable layout.
 */
typedef struct {
    uint64_t size;
    OffsetPtr next_free;
    uint32_t flags;
    uint32_t payload_offset;
} AllocatorBlockHeader;

enum {
    OFFSET_STORE_ALLOCATOR_VERSION = 1,
    OFFSET_STORE_ALLOCATOR_BLOCK_FREE = 1u
};

bool allocator_init(ShmRegion *region);
const AllocatorHeader *allocator_header(const ShmRegion *region);
bool allocator_validate(const ShmRegion *region);
bool allocator_alloc(ShmRegion *region, size_t size, size_t alignment, void **out_ptr);
bool allocator_free(ShmRegion *region, void *ptr);
bool allocator_allocation_span(const ShmRegion *region, const void *ptr, size_t *out_size);

#endif
