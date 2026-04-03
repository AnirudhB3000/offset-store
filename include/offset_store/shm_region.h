#ifndef OFFSET_STORE_SHM_REGION_H
#define OFFSET_STORE_SHM_REGION_H

#include "offset_store/offset_store.h"
#include "offset_store/offset_ptr.h"

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>

/**
 * @file shm_region.h
 * @brief Shared-memory region lifecycle, synchronization, and discovery APIs.
 */

/**
 * @name Process-Local Descriptors
 *
 * These types describe one process's view of a shared-memory mapping. They are
 * never safe to store inside the shared region itself because they include
 * process-local resources such as file descriptors and virtual addresses.
 */
/**@{*/

/**
 * @brief Process-local descriptor for one shared-memory mapping.
 *
 * This descriptor tracks the file descriptor and mapping details for a shared
 * region but is never stored inside shared memory.
 */
typedef struct {
    /** Open file descriptor for the shared-memory object, or `-1` when closed. */
    int fd;
    /** Base address returned by `mmap`. */
    void *base;
    /** Total size of the mapped region in bytes. */
    size_t size;
    /** Whether this process created the region rather than attaching to it. */
    bool creator;
} ShmRegion;

/**@}*/

/**
 * @brief Public region layout version stored in the shared header.
 */
enum {
    OFFSET_STORE_REGION_VERSION = 6
};

/**
 * @brief Fixed root-table limits stored in the private shared region header.
 *
 * Root names are stored inline in a fixed-capacity table so root discovery
 * remains deterministic and does not require allocator recursion.
 */
enum {
    /** Maximum number of named roots stored in one region. */
    OFFSET_STORE_ROOT_CAPACITY = 16,
    /** Maximum root-name length including the terminating null byte. */
    OFFSET_STORE_ROOT_NAME_LENGTH = 32
};

/**
 * @brief Fixed index-table limits stored in the private shared region header.
 */
enum {
    /** Maximum number of indexed entries stored in one region. */
    OFFSET_STORE_INDEX_CAPACITY = 32,
    /** Maximum index-key length including the terminating null byte. */
    OFFSET_STORE_INDEX_KEY_LENGTH = 32
};

/**
 * @name Region Lifecycle
 * @{
 */

/**
 * @brief Creates and maps a new shared-memory region.
 *
 * @param[out] out_region Process-local descriptor to initialize.
 * @param name POSIX shared-memory object name.
 * @param size Requested region size in bytes.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_create(ShmRegion *out_region, const char *name, size_t size);
/**
 * @brief Opens and maps an existing shared-memory region.
 *
 * @param[out] out_region Process-local descriptor to initialize.
 * @param name POSIX shared-memory object name.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_open(ShmRegion *out_region, const char *name);
/**
 * @brief Unmaps and closes a process-local region descriptor.
 *
 * @param region Region descriptor to close.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_close(ShmRegion *region);
/**
 * @brief Removes a POSIX shared-memory object from the system namespace.
 *
 * @param name POSIX shared-memory object name.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_unlink(const char *name);

/** @} */

/**
 * @name Synchronization
 * @{
 */

/**
 * @brief Acquires the allocator subsystem mutex.
 *
 * This backward-compatible helper now maps to the allocator lock so existing
 * callers that need explicit allocator serialization continue to work.
 * On platforms that support robust process-shared mutexes, owner death is
 * surfaced as `OFFSET_STORE_STATUS_INVALID_STATE` after the mutex has been
 * marked consistent and released so callers can validate or repair state
 * before retrying.
 *
 * @param region Region whose allocator mutex should be locked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_lock(ShmRegion *region);
/**
 * @brief Releases the allocator subsystem mutex.
 *
 * This backward-compatible helper now maps to the allocator lock so existing
 * callers that need explicit allocator serialization continue to work.
 *
 * @param region Region whose allocator mutex should be unlocked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_unlock(ShmRegion *region);
/**
 * @brief Acquires the allocator subsystem mutex.
 *
 * On platforms that support robust process-shared mutexes, owner death is
 * surfaced as `OFFSET_STORE_STATUS_INVALID_STATE` after the mutex has been
 * marked consistent and released so callers can validate or repair state
 * before retrying.
 *
 * @param region Region whose allocator mutex should be locked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_allocator_lock(ShmRegion *region);
/**
 * @brief Releases the allocator subsystem mutex.
 *
 * @param region Region whose allocator mutex should be unlocked.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_allocator_unlock(ShmRegion *region);
/**
 * @brief Acquires the roots subsystem write lock.
 *
 * @param region Region whose roots lock should be acquired for writing.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_roots_lock(ShmRegion *region);
/**
 * @brief Acquires the roots subsystem read lock.
 *
 * @param region Region whose roots lock should be acquired for reading.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_roots_read_lock(ShmRegion *region);
/**
 * @brief Releases the roots subsystem lock.
 *
 * @param region Region whose roots lock should be released.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_roots_unlock(ShmRegion *region);
/**
 * @brief Acquires the index subsystem write lock.
 *
 * @param region Region whose index lock should be acquired for writing.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_lock(ShmRegion *region);
/**
 * @brief Acquires the index subsystem read lock.
 *
 * @param region Region whose index lock should be acquired for reading.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_read_lock(ShmRegion *region);
/**
 * @brief Releases the index subsystem lock.
 *
 * @param region Region whose index lock should be released.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_unlock(ShmRegion *region);
/**
 * @brief Validates the private shared region header for an attached mapping.
 *
 * This check verifies that the mapped region contains the expected magic,
 * layout version, state flags, generation counter, header checksum,
 * total-size metadata, an operational allocator mutex, and operational
 * roots/index rwlocks for the current binary.
 *
 * @param region Region descriptor to validate.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_validate(const ShmRegion *region);

/** @} */

/**
 * @name Region Introspection
 * @{
 */

/**
 * @brief Returns the size of the private shared region header.
 *
 * @return Header size in bytes.
 */
size_t shm_region_header_size(void);
/**
 * @brief Returns the total mapped size recorded in the shared header.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_total_size Total size in bytes on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_get_total_size(const ShmRegion *region, uint64_t *out_total_size);
/**
 * @brief Returns the layout version recorded in the shared header.
 *
 * @param region Region descriptor to inspect.
 * @param[out] out_version Region version on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_get_version(const ShmRegion *region, uint32_t *out_version);
/**
 * @brief Returns the first byte after the private shared region header.
 *
 * The returned pointer is process-local and must not be stored inside
 * shared-memory-resident metadata.
 *
 * @param region Region descriptor to inspect.
 * @return Pointer to the usable data portion of the mapping, or `NULL` on failure.
 */
void *shm_region_data(const ShmRegion *region);
/**
 * @brief Returns a read-only pointer to the first byte after the private shared region header.
 *
 * The returned pointer is process-local and must not be stored inside
 * shared-memory-resident metadata.
 *
 * @param region Region descriptor to inspect.
 * @return Const pointer to the usable data portion of the mapping, or `NULL` on failure.
 */
const void *shm_region_data_const(const ShmRegion *region);
/**
 * @brief Returns the number of usable bytes after the private shared header.
 *
 * @param region Region descriptor to inspect.
 * @return Usable payload size in bytes, or zero on invalid input.
 */
size_t shm_region_usable_size(const ShmRegion *region);

/** @} */

/**
 * @name Root Directory
 * @{
 */

/**
 * @brief Stores or replaces a named root entry inside the shared region.
 *
 * This helper acquires the roots write lock internally and updates the fixed root
 * table stored in the private shared header without taking the allocator lock.
 *
 * @param region Region descriptor whose root table should be updated.
 * @param name Root name to create or replace.
 * @param object Offset handle stored for the root.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_set_root(ShmRegion *region, const char *name, OffsetPtr object);
/**
 * @brief Looks up a named root entry inside the shared region.
 *
 * This helper acquires the roots read lock internally before reading the fixed root
 * table stored in the private shared header.
 *
 * @param region Region descriptor whose root table should be queried.
 * @param name Root name to resolve.
 * @param[out] out_object Stored root handle on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_get_root(ShmRegion *region, const char *name, OffsetPtr *out_object);
/**
 * @brief Removes a named root entry from the shared region.
 *
 * This helper acquires the roots write lock internally before clearing the fixed
 * root table entry stored in the private shared header.
 *
 * @param region Region descriptor whose root table should be updated.
 * @param name Root name to remove.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_remove_root(ShmRegion *region, const char *name);

/** @} */

/**
 * @name Shared Index
 * @{
 */

/**
 * @brief Stores or replaces an indexed entry inside the shared region.
 *
 * This helper acquires the index write lock internally and updates the fixed index
 * table stored in the private shared header without taking the allocator lock.
 *
 * @param region Region descriptor whose index should be updated.
 * @param key Index key to create or replace.
 * @param object Offset handle stored for the key.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_put(ShmRegion *region, const char *key, OffsetPtr object);
/**
 * @brief Looks up an indexed entry inside the shared region.
 *
 * This helper acquires the index read lock internally before reading the fixed
 * index table stored in the private shared header.
 *
 * @param region Region descriptor whose index should be queried.
 * @param key Index key to resolve.
 * @param[out] out_object Stored handle on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_get(ShmRegion *region, const char *key, OffsetPtr *out_object);
/**
 * @brief Returns whether an indexed entry is present in the shared region.
 *
 * @param region Region descriptor whose index should be queried.
 * @param key Index key to test.
 * @param[out] out_contains `true` if the key is present.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_contains(ShmRegion *region, const char *key, bool *out_contains);
/**
 * @brief Removes an indexed entry from the shared region.
 *
 * This helper acquires the index write lock internally before clearing the fixed
 * index entry stored in the private shared header.
 *
 * @param region Region descriptor whose index should be updated.
 * @param key Index key to remove.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus shm_region_index_remove(ShmRegion *region, const char *key);

/** @} */

#endif
