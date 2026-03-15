#include "offset_store/object_store.h"

#include "offset_store/allocator.h"

#include <errno.h>
#include <stdalign.h>

_Static_assert(
    (sizeof(ObjectHeader) % alignof(max_align_t)) == 0,
    "ObjectHeader size must preserve payload alignment"
);

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
    if (!allocator_allocation_span(region, *out_header, &allocation_size)) {
        return false;
    }

    return total_size <= allocation_size;
}

const ObjectHeader *object_store_header(const ShmRegion *region, OffsetPtr object)
{
    void *resolved;

    if (!object_store_resolve_header(region, object, 0, &resolved)) {
        return NULL;
    }

    return (const ObjectHeader *) resolved;
}

ObjectHeader *object_store_header_mut(ShmRegion *region, OffsetPtr object)
{
    void *resolved;

    if (!object_store_resolve_header(region, object, 0, &resolved)) {
        return NULL;
    }

    return (ObjectHeader *) resolved;
}

const void *object_store_payload_const(const ShmRegion *region, OffsetPtr object)
{
    const ObjectHeader *header;
    void *resolved;

    header = object_store_header(region, object);
    if (header == NULL) {
        return NULL;
    }

    if (!object_store_resolve_header(region, object, header->size, &resolved)) {
        return NULL;
    }

    return (const unsigned char *) resolved + sizeof(ObjectHeader);
}

void *object_store_payload(ShmRegion *region, OffsetPtr object)
{
    return (void *) object_store_payload_const(region, object);
}

bool object_store_alloc(ShmRegion *region, uint32_t type, size_t payload_size, OffsetPtr *out_object)
{
    void *storage;
    ObjectHeader *header;

    if (region == NULL || out_object == NULL || payload_size > UINT32_MAX) {
        errno = EINVAL;
        return false;
    }

    if (!allocator_alloc(region, sizeof(ObjectHeader) + payload_size, alignof(max_align_t), &storage)) {
        return false;
    }

    header = (ObjectHeader *) storage;
    header->size = (uint32_t) payload_size;
    header->type = type;
    header->flags = 0;
    header->reserved = 0;

    if (!offset_ptr_try_from_raw(region->base, region->size, header, out_object)) {
        errno = EINVAL;
        return false;
    }

    return true;
}

bool object_store_free(ShmRegion *region, OffsetPtr object)
{
    ObjectHeader *header;

    if (region == NULL || offset_ptr_is_null(object)) {
        errno = EINVAL;
        return false;
    }

    header = object_store_header_mut(region, object);
    if (header == NULL) {
        errno = EINVAL;
        return false;
    }

    if (!object_store_resolve_header(region, object, header->size, (void **) &header)) {
        errno = EINVAL;
        return false;
    }

    return allocator_free(region, header);
}
