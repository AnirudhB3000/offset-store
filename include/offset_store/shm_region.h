#ifndef OFFSET_STORE_SHM_REGION_H
#define OFFSET_STORE_SHM_REGION_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

/*
 * The region header lives at the front of the shared mapping so attachers can
 * validate that they are looking at an offset-store region with the expected
 * layout version.
 */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    uint64_t total_size;
    pthread_mutex_t mutex;
} ShmRegionHeader;

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

bool shm_region_create(ShmRegion *out_region, const char *name, size_t size);
bool shm_region_open(ShmRegion *out_region, const char *name);
bool shm_region_close(ShmRegion *region);
bool shm_region_unlink(const char *name);
bool shm_region_lock(ShmRegion *region);
bool shm_region_unlock(ShmRegion *region);

const ShmRegionHeader *shm_region_header(const ShmRegion *region);
ShmRegionHeader *shm_region_header_mut(ShmRegion *region);
void *shm_region_data(const ShmRegion *region);
size_t shm_region_usable_size(const ShmRegion *region);

#endif
