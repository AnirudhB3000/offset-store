#ifndef OFFSET_STORE_STORE_H
#define OFFSET_STORE_STORE_H

#include "offset_store/allocator.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @file store.h
 * @brief High-level shared-memory store lifecycle and discovery APIs.
 */

/**
 * @name Process-Local Descriptors
 *
 * These wrappers exist only inside one process. They coordinate access to a
 * shared region but are not themselves part of the shared-memory layout.
 */
/**@{*/

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

/**@}*/

/**
 * @name Store Lifecycle
 * @{
 */

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
/**
 * @brief Validates the shared region and allocator state of an opened store.
 *
 * This high-level validation checks the region header and allocator metadata
 * together so callers can verify that an attached store is structurally usable.
 *
 * @param store Store descriptor to validate.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_validate(const OffsetStore *store);

/** @} */

/**
 * @name Named Roots
 * @{
 */
/**
 * @brief Stores or replaces a named root in the shared region.
 *
 * Root entries provide stable discovery names for shared objects without
 * requiring processes to exchange raw offsets out of band.
 *
 * @param store Store descriptor whose root table should be updated.
 * @param name Root name to create or replace.
 * @param object Object handle to associate with the root.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_set_root(OffsetStore *store, const char *name, OffsetPtr object);
/**
 * @brief Resolves a named root to its stored object handle.
 *
 * @param store Store descriptor whose root table should be queried.
 * @param name Root name to resolve.
 * @param[out] out_object Stored object handle on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_get_root(OffsetStore *store, const char *name, OffsetPtr *out_object);
/**
 * @brief Removes a named root from the shared region.
 *
 * @param store Store descriptor whose root table should be updated.
 * @param name Root name to remove.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_remove_root(OffsetStore *store, const char *name);

/** @} */

/**
 * @name Shared Index
 * @{
 */
/**
 * @brief Stores or replaces an indexed entry in the shared region.
 *
 * Indexed entries provide a general fixed-capacity directory for shared object
 * handles beyond the small set of well-known root names.
 *
 * @param store Store descriptor whose index should be updated.
 * @param key Index key to create or replace.
 * @param object Object handle to associate with the key.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_index_put(OffsetStore *store, const char *key, OffsetPtr object);
/**
 * @brief Resolves an indexed entry to its stored object handle.
 *
 * @param store Store descriptor whose index should be queried.
 * @param key Index key to resolve.
 * @param[out] out_object Stored object handle on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_index_get(OffsetStore *store, const char *key, OffsetPtr *out_object);
/**
 * @brief Returns whether an index key is currently present.
 *
 * @param store Store descriptor whose index should be queried.
 * @param key Index key to test.
 * @param[out] out_contains `true` if the key is present.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_index_contains(OffsetStore *store, const char *key, bool *out_contains);
/**
 * @brief Removes an indexed entry from the shared region.
 *
 * @param store Store descriptor whose index should be updated.
 * @param key Index key to remove.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus offset_store_index_remove(OffsetStore *store, const char *key);

/** @} */

#endif
