#ifndef OFFSET_STORE_STORE_H
#define OFFSET_STORE_STORE_H

#include "offset_store/allocator.h"
#include "offset_store/shm_region.h"

#include <stddef.h>

/**
 * @brief Process-local convenience wrapper for common store lifecycle flows.
 *
 * `OffsetStore` is a convenience wrapper for the common lifecycle path: create
 * or attach the region, ensure allocator state is available, and expose the
 * underlying shared region to higher-level modules.
 */
typedef struct {
    /** Process-local shared region descriptor. */
    ShmRegion region;
} OffsetStore;

/**
 * @brief Creates a region and initializes allocator state for first use.
 *
 * @param[out] store Store descriptor to initialize.
 * @param name POSIX shared-memory object name.
 * @param size Requested region size in bytes.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_bootstrap(OffsetStore *store, const char *name, size_t size);
/**
 * @brief Opens an existing initialized store.
 *
 * @param[out] store Store descriptor to initialize.
 * @param name POSIX shared-memory object name.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_open_existing(OffsetStore *store, const char *name);
/**
 * @brief Closes a process-local store wrapper.
 *
 * @param store Store descriptor to close.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_close(OffsetStore *store);

#endif
