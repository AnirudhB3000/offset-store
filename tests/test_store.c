#define _POSIX_C_SOURCE 200809L

#include "offset_store/object_store.h"
#include "offset_store/store.h"

#include <stdio.h>
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
 * @brief Builds a unique shared-memory name for one lifecycle test.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    int written;

    /*
     * The lifecycle helpers create global POSIX shared-memory names, so tests
     * include the PID to keep runs isolated from each other.
     */
    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Verifies that bootstrap creates a store with valid allocator state.
 */
static void test_bootstrap_initializes_allocator_state(void)
{
    char name[64];
    OffsetStore store;

    make_region_name(name, sizeof(name), "store-bootstrap");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&store.region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that bootstrap is one-shot for a given shared-memory name.
 */
static void test_bootstrap_is_one_shot_per_region_name(void)
{
    char name[64];
    OffsetStore first;
    OffsetStore second;

    make_region_name(name, sizeof(name), "store-one-shot");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&first, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_ALREADY_EXISTS, offset_store_bootstrap(&second, name, 4096));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&first));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that opening an existing store attaches to initialized state.
 */
static void test_open_existing_attaches_to_initialized_store(void)
{
    char name[64];
    OffsetStore creator;
    OffsetStore attached;
    void *allocation;

    make_region_name(name, sizeof(name), "store-open");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&creator.region, 64, 16, &allocation));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&attached.region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that opening rejects a region without allocator metadata.
 */
static void test_open_existing_rejects_region_without_allocator_state(void)
{
    char name[64];
    OffsetStore attached;
    ShmRegion region;

    make_region_name(name, sizeof(name), "store-invalid");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, offset_store_open_existing(&attached, name));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies whole-store validation covers the region header and allocator state.
 */
static void test_store_validate_checks_region_and_allocator(void)
{
    char name[64];
    OffsetStore store;
    uint64_t *magic;

    make_region_name(name, sizeof(name), "store-validate");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_validate(&store));

    magic = (uint64_t *) store.region.base;
    TEST_ASSERT_NOT_NULL(magic);
    *magic = 0;
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, offset_store_validate(&store));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that a named root can be stored, resolved, and removed.
 */
static void test_named_roots_round_trip(void)
{
    char name[64];
    OffsetStore store;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;

    make_region_name(name, sizeof(name), "store-roots-roundtrip");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 7, 32, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&store, "message", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_get_root(&store, "message", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_remove_root(&store, "message"));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_NOT_FOUND,
        offset_store_get_root(&store, "message", &resolved_object)
    );

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that named roots are visible to later attached processes.
 */
static void test_open_existing_observes_named_roots(void)
{
    char name[64];
    OffsetStore creator;
    OffsetStore attached;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;

    make_region_name(name, sizeof(name), "store-roots-attach");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&creator.region, 9, 48, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&creator, "shared", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_get_root(&attached, "shared", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that root creation replaces an existing name and validates inputs.
 */
static void test_named_roots_replace_existing_binding_and_validate_input(void)
{
    char name[64];
    char too_long_name[OFFSET_STORE_ROOT_NAME_LENGTH + 4];
    OffsetStore store;
    OffsetPtr first_object;
    OffsetPtr second_object;
    OffsetPtr resolved_object;

    memset(too_long_name, 'a', sizeof(too_long_name));
    too_long_name[sizeof(too_long_name) - 1] = '\0';

    make_region_name(name, sizeof(name), "store-roots-replace");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 11, 16, &first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 12, 16, &second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&store, "replaceable", first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&store, "replaceable", second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_get_root(&store, "replaceable", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(second_object.offset, resolved_object.offset);
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_set_root(&store, too_long_name, second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_set_root(&store, "null-object", offset_ptr_null())
    );

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that indexed entries can be stored, resolved, queried, and removed.
 */
static void test_index_round_trip(void)
{
    char name[64];
    OffsetStore store;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;
    bool contains;

    make_region_name(name, sizeof(name), "store-index-roundtrip");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 13, 32, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&store, "message-1", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_contains(&store, "message-1", &contains)
    );
    TEST_ASSERT_TRUE(contains);
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_get(&store, "message-1", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_index_remove(&store, "message-1"));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_contains(&store, "message-1", &contains)
    );
    TEST_ASSERT_FALSE(contains);
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_NOT_FOUND,
        offset_store_index_get(&store, "message-1", &resolved_object)
    );

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that indexed entries are visible to later attached processes.
 */
static void test_open_existing_observes_index_entries(void)
{
    char name[64];
    OffsetStore creator;
    OffsetStore attached;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;

    make_region_name(name, sizeof(name), "store-index-attach");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&creator.region, 14, 48, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&creator, "shared-1", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_get(&attached, "shared-1", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies index replacement, capacity limits, and input validation.
 */
static void test_index_replace_capacity_and_validate_input(void)
{
    char name[64];
    char too_long_key[OFFSET_STORE_INDEX_KEY_LENGTH + 4];
    OffsetStore store;
    OffsetPtr first_object;
    OffsetPtr second_object;
    OffsetPtr temp_object;
    OffsetPtr resolved_object;
    bool contains;
    char key[32];
    int written;
    size_t index;

    memset(too_long_key, 'b', sizeof(too_long_key));
    too_long_key[sizeof(too_long_key) - 1] = '\0';

    make_region_name(name, sizeof(name), "store-index-validate");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 8192));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 15, 16, &first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 16, 16, &second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&store, "replaceable-1", first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&store, "replaceable-1", second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_get(&store, "replaceable-1", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(second_object.offset, resolved_object.offset);

    for (index = 0; index < OFFSET_STORE_INDEX_CAPACITY - 1; ++index) {
        written = snprintf(key, sizeof(key), "k%zu", index);
        TEST_ASSERT_TRUE(written > 0);
        TEST_ASSERT_TRUE((size_t) written < sizeof(key));
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            object_store_alloc(&store.region, (uint32_t) (100 + index), 8, &temp_object)
        );
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            offset_store_index_put(&store, key, temp_object)
        );
    }

    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 999, 8, &temp_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OUT_OF_MEMORY,
        offset_store_index_put(&store, "overflow-key", temp_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_index_put(&store, too_long_key, second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_index_put(&store, "null-object-1", offset_ptr_null())
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_index_contains(&store, "replaceable-1", NULL)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_contains(&store, "missing-1", &contains)
    );
    TEST_ASSERT_FALSE(contains);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Runs the store lifecycle unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bootstrap_initializes_allocator_state);
    RUN_TEST(test_bootstrap_is_one_shot_per_region_name);
    RUN_TEST(test_open_existing_attaches_to_initialized_store);
    RUN_TEST(test_open_existing_rejects_region_without_allocator_state);
    RUN_TEST(test_store_validate_checks_region_and_allocator);
    RUN_TEST(test_named_roots_round_trip);
    RUN_TEST(test_open_existing_observes_named_roots);
    RUN_TEST(test_named_roots_replace_existing_binding_and_validate_input);
    RUN_TEST(test_index_round_trip);
    RUN_TEST(test_open_existing_observes_index_entries);
    RUN_TEST(test_index_replace_capacity_and_validate_input);
    return UNITY_END();
}
