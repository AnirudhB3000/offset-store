#ifndef OFFSET_STORE_SHM_REGION_H
#define OFFSET_STORE_SHM_REGION_H

#include "offset_store/offset_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

/*
 * This descriptor is process-local. It tracks the file descriptor and mapping
 * details for a shared region but is never stored inside shared memory.
 */
typedef struct {
    int fd;
    void *base;
    size_t size;
    bool creator;
} ShmRegion;

enum {
    OFFSET_STORE_REGION_VERSION = 1
};

OffsetStoreStatus shm_region_create(ShmRegion *out_region, const char *name, size_t size);
OffsetStoreStatus shm_region_open(ShmRegion *out_region, const char *name);
OffsetStoreStatus shm_region_close(ShmRegion *region);
OffsetStoreStatus shm_region_unlink(const char *name);
OffsetStoreStatus shm_region_lock(ShmRegion *region);
OffsetStoreStatus shm_region_unlock(ShmRegion *region);

size_t shm_region_header_size(void);
OffsetStoreStatus shm_region_total_size(const ShmRegion *region, uint64_t *out_total_size);
OffsetStoreStatus shm_region_version(const ShmRegion *region, uint32_t *out_version);
void *shm_region_data(const ShmRegion *region);
size_t shm_region_usable_size(const ShmRegion *region);

#endif
