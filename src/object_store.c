#include "offset_store/object_store.h"

#include "offset_store/allocator.h"

#include <stdalign.h>

_Static_assert(
    (sizeof(ObjectHeader) % alignof(max_align_t)) == 0,
    "ObjectHeader size must preserve payload alignment"
);

/**
 * @brief Resolves and validates an object header plus an optional payload span.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to the object header.
 * @param payload_size Additional payload bytes that must fit after the header.
 * @param[out] out_header Resolved header pointer on success.
 * @return true if the object resolves to the start of a live allocation.
 * @return false otherwise.
 */
static bool object_store_resolve_header(
    const ShmRegion *region,
    OffsetPtr object,
    size_t payload_size,
    void **out_header
)
{
    size_t allocation_size;
    size_t total_size;

    if (region == NULL || out_header == NULL || offset_ptr_is_null(object)) {
        return false;
    }

    if (payload_size > SIZE_MAX - sizeof(ObjectHeader)) {
        return false;
    }

    total_size = sizeof(ObjectHeader) + payload_size;
    if (!offset_ptr_try_resolve(region->base, region->size, object, total_size, out_header)) {
        return false;
    }

    /* Object handles must point to the start of a live allocator allocation. */
    if (allocator_get_allocation_span(region, *out_header, &allocation_size) != OFFSET_STORE_STATUS_OK) {
        return false;
    }

    if (total_size > allocation_size) {
        return false;
    }

    return ((((const ObjectHeader *) *out_header)->flags & OFFSET_STORE_OBJECT_FLAG_FREED) == 0u);
}

/**
 * @brief Resolves an object handle to a read-only header pointer.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to resolve.
 * @return Read-only header pointer on success, or `NULL` on failure.
 */
const ObjectHeader *object_store_get_header(const ShmRegion *region, OffsetPtr object)
{
    void *resolved;

    if (!object_store_resolve_header(region, object, 0, &resolved)) {
        return NULL;
    }

    return (const ObjectHeader *) resolved;
}

/**
 * @brief Resolves an object handle to a mutable header pointer.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to resolve.
 * @return Mutable header pointer on success, or `NULL` on failure.
 */
ObjectHeader *object_store_get_header_mut(ShmRegion *region, OffsetPtr object)
{
    void *resolved;

    if (!object_store_resolve_header(region, object, 0, &resolved)) {
        return NULL;
    }

    return (ObjectHeader *) resolved;
}

/**
 * @brief Resolves an object handle to a read-only payload pointer.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to resolve.
 * @return Read-only payload pointer on success, or `NULL` on failure.
 */
const void *object_store_get_payload_const(const ShmRegion *region, OffsetPtr object)
{
    const ObjectHeader *header;
    void *resolved;

    header = object_store_get_header(region, object);
    if (header == NULL) {
        return NULL;
    }

    if (!object_store_resolve_header(region, object, header->size, &resolved)) {
        return NULL;
    }

    return (const unsigned char *) resolved + sizeof(ObjectHeader);
}

/**
 * @brief Resolves an object handle to a mutable payload pointer.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to resolve.
 * @return Mutable payload pointer on success, or `NULL` on failure.
 */
void *object_store_get_payload(ShmRegion *region, OffsetPtr object)
{
    return (void *) object_store_get_payload_const(region, object);
}

/**
 * @brief Allocates a new object and returns its offset handle.
 *
 * @param region Region whose allocator should satisfy the request.
 * @param type Caller-defined object type identifier.
 * @param payload_size Requested payload size in bytes.
 * @param[out] out_object Offset handle on success.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus object_store_alloc(ShmRegion *region, uint32_t type, size_t payload_size, OffsetPtr *out_object)
{
    OffsetStoreStatus status;
    void *storage;
    ObjectHeader *header;

    if (region == NULL || out_object == NULL || payload_size > UINT32_MAX) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    status = allocator_alloc(region, sizeof(ObjectHeader) + payload_size, alignof(max_align_t), &storage);
    if (status != OFFSET_STORE_STATUS_OK) {
        return status;
    }

    header = (ObjectHeader *) storage;
    header->size = (uint32_t) payload_size;
    header->type = type;
    header->flags = 0;
    header->reserved = 0;

    if (!offset_ptr_try_from_raw(region->base, region->size, header, out_object)) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Frees an object previously allocated from the object store.
 *
 * @param region Region whose allocator owns the object.
 * @param object Offset handle to free.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus object_store_free(ShmRegion *region, OffsetPtr object)
{
    ObjectHeader *header;
    size_t payload_size;

    if (region == NULL || offset_ptr_is_null(object)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = object_store_get_header_mut(region, object);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_NOT_FOUND;
    }

    if (!object_store_resolve_header(region, object, header->size, (void **) &header)) {
        return OFFSET_STORE_STATUS_NOT_FOUND;
    }

    payload_size = header->size;
    header->flags |= OFFSET_STORE_OBJECT_FLAG_FREED;
    header->type = 0;
    header->reserved = UINT32_C(0x46524545);
    header->size = (uint32_t) payload_size;

    return allocator_free(region, header);
}

/**
 * @brief Validates that an object handle resolves to a live well-formed object.
 *
 * @param region Region whose mapping contains the object.
 * @param object Offset handle to validate.
 * @return Status code describing success or failure.
 */
OffsetStoreStatus object_store_validate(const ShmRegion *region, OffsetPtr object)
{
    const ObjectHeader *header;

    if (region == NULL || offset_ptr_is_null(object)) {
        return OFFSET_STORE_STATUS_INVALID_ARGUMENT;
    }

    header = object_store_get_header(region, object);
    if (header == NULL) {
        return OFFSET_STORE_STATUS_NOT_FOUND;
    }

    return object_store_resolve_header(region, object, header->size, (void **) &header)
        ? OFFSET_STORE_STATUS_OK
        : OFFSET_STORE_STATUS_INVALID_STATE;
}
