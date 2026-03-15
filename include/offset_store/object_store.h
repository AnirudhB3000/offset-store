#ifndef OFFSET_STORE_OBJECT_STORE_H
#define OFFSET_STORE_OBJECT_STORE_H

#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Objects begin with a fixed-size header so the payload can follow immediately
 * while preserving a stable layout across processes.
 */
typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t flags;
    uint32_t reserved;
} ObjectHeader;

bool object_store_alloc(ShmRegion *region, uint32_t type, size_t payload_size, OffsetPtr *out_object);
bool object_store_free(ShmRegion *region, OffsetPtr object);

const ObjectHeader *object_store_header(const ShmRegion *region, OffsetPtr object);
ObjectHeader *object_store_header_mut(ShmRegion *region, OffsetPtr object);
const void *object_store_payload_const(const ShmRegion *region, OffsetPtr object);
void *object_store_payload(ShmRegion *region, OffsetPtr object);

#endif
