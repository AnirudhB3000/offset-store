#define _POSIX_C_SOURCE 200809L

#include "offset_store/object_store.h"

#include "offset_store/allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdalign.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"

/**
 * @brief Provides per-test setup for Unity.
 */
void setUp(void)
{
}

/**
 * @brief Provides per-test teardown for Unity.
 */
void tearDown(void)
{
}

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
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Verifies the fixed object header size and alignment contract.
 */
static void test_object_header_layout(void)
{
    TEST_ASSERT_EQUAL_UINT64(16, sizeof(ObjectHeader));
    TEST_ASSERT_EQUAL_UINT64(0, sizeof(ObjectHeader) % alignof(max_align_t));
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
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&region, 7, 32, &object));

    header = object_store_get_header_mut(&region, object);
    TEST_ASSERT_NOT_NULL(header);
    TEST_ASSERT_EQUAL_UINT32(7, header->type);
    TEST_ASSERT_EQUAL_UINT64(32, header->size);

    payload = (uint8_t *) object_store_get_payload(&region, object);
    TEST_ASSERT_NOT_NULL(payload);
    memset(payload, 0xab, 32);
    TEST_ASSERT_EQUAL_UINT8(0xab, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(0xab, payload[31]);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
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
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&creator_region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&creator_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&creator_region, 9, 8, &object));

    creator_payload = (uint8_t *) object_store_get_payload(&creator_region, object);
    TEST_ASSERT_NOT_NULL(creator_payload);
    creator_payload[0] = 0x11;
    creator_payload[1] = 0x22;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_open(&attached_region, name));
    attached_payload = (const uint8_t *) object_store_get_payload_const(&attached_region, object);
    TEST_ASSERT_NOT_NULL(attached_payload);
    TEST_ASSERT_EQUAL_UINT8(0x11, attached_payload[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22, attached_payload[1]);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&attached_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&creator_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
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
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&region, 1, 24, &first));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_free(&region, first));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&region, 2, 24, &second));
    TEST_ASSERT_EQUAL_UINT64(first.offset, second.offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
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
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    invalid.offset = 8;
    TEST_ASSERT_NULL(object_store_get_header(&region, invalid));
    TEST_ASSERT_NULL(object_store_get_payload_const(&region, invalid));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_NOT_FOUND, object_store_free(&region, invalid));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies getter-style header and payload accessors agree on the same object.
 */
static void test_object_getters_are_consistent(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr object;
    const ObjectHeader *const_header;
    ObjectHeader *mutable_header;
    const uint8_t *const_payload;
    uint8_t *mutable_payload;

    make_region_name(name, sizeof(name), "object-getters");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&region, 17, 40, &object));

    const_header = object_store_get_header(&region, object);
    mutable_header = object_store_get_header_mut(&region, object);
    const_payload = (const uint8_t *) object_store_get_payload_const(&region, object);
    mutable_payload = (uint8_t *) object_store_get_payload(&region, object);

    TEST_ASSERT_NOT_NULL(const_header);
    TEST_ASSERT_NOT_NULL(mutable_header);
    TEST_ASSERT_NOT_NULL(const_payload);
    TEST_ASSERT_NOT_NULL(mutable_payload);
    TEST_ASSERT_EQUAL_PTR(const_header, mutable_header);
    TEST_ASSERT_EQUAL_PTR(const_payload, mutable_payload);
    TEST_ASSERT_EQUAL_PTR(
        (const unsigned char *) const_header + sizeof(ObjectHeader),
        const_payload
    );

    mutable_header->flags = 0x12345678u;
    mutable_payload[0] = 0x5c;
    TEST_ASSERT_EQUAL_HEX32(0x12345678u, const_header->flags);
    TEST_ASSERT_EQUAL_UINT8(0x5c, const_payload[0]);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies object getter-style accessors reject invalid arguments consistently.
 */
static void test_object_getters_reject_invalid_arguments(void)
{
    OffsetPtr null_object;

    null_object.offset = 0;

    TEST_ASSERT_NULL(object_store_get_header(NULL, null_object));
    TEST_ASSERT_NULL(object_store_get_header_mut(NULL, null_object));
    TEST_ASSERT_NULL(object_store_get_payload_const(NULL, null_object));
    TEST_ASSERT_NULL(object_store_get_payload(NULL, null_object));
}

/**
 * @brief Runs the object-store unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_object_header_layout);
    RUN_TEST(test_object_alloc_and_resolve);
    RUN_TEST(test_object_offset_is_stable_across_attach);
    RUN_TEST(test_object_free_releases_storage);
    RUN_TEST(test_object_rejects_invalid_offset);
    RUN_TEST(test_object_getters_are_consistent);
    RUN_TEST(test_object_getters_reject_invalid_arguments);
    return UNITY_END();
}
