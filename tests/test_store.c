#define _POSIX_C_SOURCE 200809L

#include "offset_store/store.h"

#include <stdio.h>
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
    return UNITY_END();
}
