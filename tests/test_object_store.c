#define _POSIX_C_SOURCE 200809L

#include "offset_store/object_store.h"

#include "offset_store/allocator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdalign.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Builds a unique shared-memory name for one object-store test.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    /*
     * Shared-memory objects are visible system-wide, so tests use the process
     * ID in the name to avoid collisions.
     */
    int written;

    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    assert(written > 0);
    assert((size_t) written < buffer_size);
}

/**
 * @brief Verifies the fixed object header size and alignment contract.
 */
static void test_object_header_layout(void)
{
    assert(sizeof(ObjectHeader) == 16);
    assert((sizeof(ObjectHeader) % alignof(max_align_t)) == 0);
}

/**
 * @brief Verifies object allocation and payload resolution.
 */
static void test_object_alloc_and_resolve(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr object;
    ObjectHeader *header;
    uint8_t *payload;

    make_region_name(name, sizeof(name), "object-alloc");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);
    assert(object_store_alloc(&region, 7, 32, &object) == OFFSET_STORE_STATUS_OK);

    header = object_store_get_header_mut(&region, object);
    assert(header != NULL);
    assert(header->type == 7);
    assert(header->size == 32);

    payload = (uint8_t *) object_store_get_payload(&region, object);
    assert(payload != NULL);
    memset(payload, 0xab, 32);
    assert(payload[0] == 0xab);
    assert(payload[31] == 0xab);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies object offsets remain valid across separate attaches.
 */
static void test_object_offset_is_stable_across_attach(void)
{
    char name[64];
    ShmRegion creator_region;
    ShmRegion attached_region;
    OffsetPtr object;
    uint8_t *creator_payload;
    const uint8_t *attached_payload;

    make_region_name(name, sizeof(name), "object-attach");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&creator_region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&creator_region) == OFFSET_STORE_STATUS_OK);
    assert(object_store_alloc(&creator_region, 9, 8, &object) == OFFSET_STORE_STATUS_OK);

    creator_payload = (uint8_t *) object_store_get_payload(&creator_region, object);
    assert(creator_payload != NULL);
    creator_payload[0] = 0x11;
    creator_payload[1] = 0x22;

    assert(shm_region_open(&attached_region, name) == OFFSET_STORE_STATUS_OK);
    attached_payload = (const uint8_t *) object_store_get_payload_const(&attached_region, object);
    assert(attached_payload != NULL);
    assert(attached_payload[0] == 0x11);
    assert(attached_payload[1] == 0x22);

    assert(shm_region_close(&attached_region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_close(&creator_region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies that freeing an object releases its underlying storage.
 */
static void test_object_free_releases_storage(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr first;
    OffsetPtr second;

    make_region_name(name, sizeof(name), "object-free");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);
    assert(object_store_alloc(&region, 1, 24, &first) == OFFSET_STORE_STATUS_OK);
    assert(object_store_free(&region, first) == OFFSET_STORE_STATUS_OK);
    assert(object_store_alloc(&region, 2, 24, &second) == OFFSET_STORE_STATUS_OK);
    assert(second.offset == first.offset);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies that arbitrary invalid offsets are rejected as objects.
 */
static void test_object_rejects_invalid_offset(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr invalid;

    make_region_name(name, sizeof(name), "object-invalid");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);

    invalid.offset = 8;
    assert(object_store_get_header(&region, invalid) == NULL);
    assert(object_store_get_payload_const(&region, invalid) == NULL);
    assert(object_store_free(&region, invalid) == OFFSET_STORE_STATUS_NOT_FOUND);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Runs the object-store unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    test_object_header_layout();
    test_object_alloc_and_resolve();
    test_object_offset_is_stable_across_attach();
    test_object_free_releases_storage();
    test_object_rejects_invalid_offset();
    return 0;
}
