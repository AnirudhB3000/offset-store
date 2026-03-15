#ifndef OFFSET_STORE_STORE_H
#define OFFSET_STORE_STORE_H

#include "offset_store/allocator.h"
#include "offset_store/shm_region.h"

#include <stddef.h>

/*
 * OffsetStore is a convenience wrapper for the common lifecycle path: create or
 * attach the region, ensure allocator state is available, and expose the
 * underlying shared region to higher-level modules.
 */
typedef struct {
    ShmRegion region;
} OffsetStore;

OffsetStoreStatus offset_store_bootstrap(OffsetStore *store, const char *name, size_t size);
OffsetStoreStatus offset_store_open_existing(OffsetStore *store, const char *name);
OffsetStoreStatus offset_store_close(OffsetStore *store);

#endif
