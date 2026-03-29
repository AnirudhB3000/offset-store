#ifndef OFFSET_STORE_OBJECT_STORE_H
#define OFFSET_STORE_OBJECT_STORE_H

#include "offset_store/offset_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file object_store.h
 * @brief Shared-memory object allocation, validation, and resolution APIs.
 */

/**
 * @name Shared-Memory Resident Value Types
 *
 * These types have stable layouts and are safe to place inside allocator-owned
 * shared-memory storage.
 */
/**@{*/

/**
 * @brief Fixed header stored at the front of every object allocation.
 *
 * Objects begin with a fixed-size header so the payload can follow immediately
 * while preserving a stable layout across processes.
 */
typedef struct {
    /** Payload size in bytes. */
    uint32_t size;
    /** Caller-defined object type identifier. */
    uint32_t type;
    /** Reserved object flags for higher-level use. */
    uint32_t flags;
    /** Reserved field for future expansion. */
    uint32_t reserved;
} ObjectHeader;

/**@}*/

/**
 * @brief Public object-header flag bits reserved by the library.
 */
enum {
    /** Object has been freed and must no longer be resolved or reused by callers. */
    OFFSET_STORE_OBJECT_FLAG_FREED = 1u
};

/**
 * @name Object Lifecycle
 * @{
 */

/**
 * @brief Allocates a new object and returns its shared offset handle.
 *
 * @param region Region whose object heap should satisfy the request.
 * @param type Caller-defined object type.
 * @param payload_size Requested payload size in bytes.
 * @param[out] out_object Offset handle to the allocated object header on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus object_store_alloc(ShmRegion *region, uint32_t type, size_t payload_size, OffsetPtr *out_object);
/**
 * @brief Frees an object previously allocated from the object store.
 *
 * @param region Region whose object heap owns the object.
 * @param object Offset handle to the object header.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus object_store_free(ShmRegion *region, OffsetPtr object);
/**
 * @brief Validates that an object handle resolves to a live well-formed object.
 *
 * This check verifies that the handle is non-null, resolves within the mapped
 * region, points to the start of a live allocator allocation, and that the
 * declared payload fits within that allocation.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to validate.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus object_store_validate(const ShmRegion *region, OffsetPtr object);

/** @} */

/**
 * @name Object Resolution
 * @{
 */

/**
 * @brief Resolves an object handle to a read-only header pointer.
 *
 * The returned pointer is process-local and must not be stored back into shared
 * memory. Persist the original `OffsetPtr` instead.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to the object header.
 * @return Read-only header pointer on success, or `NULL` on failure.
 */
const ObjectHeader *object_store_get_header(const ShmRegion *region, OffsetPtr object);
/**
 * @brief Resolves an object handle to a mutable header pointer.
 *
 * The returned pointer is process-local and must not be stored back into shared
 * memory. Persist the original `OffsetPtr` instead.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to the object header.
 * @return Mutable header pointer on success, or `NULL` on failure.
 */
ObjectHeader *object_store_get_header_mut(ShmRegion *region, OffsetPtr object);
/**
 * @brief Resolves an object handle to a read-only payload pointer.
 *
 * The returned pointer is process-local and must not be stored back into shared
 * memory. Persist the original `OffsetPtr` instead.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to the object header.
 * @return Read-only payload pointer on success, or `NULL` on failure.
 */
const void *object_store_get_payload_const(const ShmRegion *region, OffsetPtr object);
/**
 * @brief Resolves an object handle to a mutable payload pointer.
 *
 * The returned pointer is process-local and must not be stored back into shared
 * memory. Persist the original `OffsetPtr` instead.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to the object header.
 * @return Mutable payload pointer on success, or `NULL` on failure.
 */
void *object_store_get_payload(ShmRegion *region, OffsetPtr object);

/** @} */

#endif
