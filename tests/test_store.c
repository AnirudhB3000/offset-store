#define _POSIX_C_SOURCE 200809L

#include "offset_store/object_store.h"
#include "offset_store/store.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "unity.h"

/**
 * @file test_store.c
 * @brief Unity tests for deterministic store lifecycle and discovery behavior.
 */

/**
 * @name Unity Lifecycle Hooks
 * @{
 */

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

/** @} */

/**
 * @name Shared Test Helpers
 * @{
 */

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
 * @brief Returns a deterministic root name for a contention reader iteration.
 *
 * @param iteration Zero-based loop iteration.
 * @return Root name string.
 */
static const char *stress_root_name_for(unsigned int iteration)
{
    static const char *names[] = {"root-a", "root-b", "root-c", "root-d"};

    return names[iteration % (sizeof(names) / sizeof(names[0]))];
}

/**
 * @brief Returns a deterministic index key for a contention reader iteration.
 *
 * @param iteration Zero-based loop iteration.
 * @return Index key string.
 */
static const char *stress_index_key_for(unsigned int iteration)
{
    static const char *keys[] = {"idx-a", "idx-b", "idx-c", "idx-d"};

    return keys[iteration % (sizeof(keys) / sizeof(keys[0]))];
}

/** @} */

/**
 * @name Stress Worker Helpers
 * @{
 */

/**
 * @brief Runs one root-writer stress worker process.
 *
 * @param store Pre-opened store descriptor inherited by the worker process.
 * @param object Object handle to publish under rotating root names.
 * @param iterations Number of update iterations to execute.
 * @param ready_fd Pipe descriptor used to signal that attach completed.
 * @param start_fd Pipe descriptor used to wait for the parent start signal.
 * @return Process exit code for the worker.
 */
static int run_root_writer_stress_worker(
    OffsetStore *store,
    OffsetPtr object,
    unsigned int iterations,
    int ready_fd,
    int start_fd
)
{
    unsigned int iteration;
    char signal_byte;

    if (store == NULL) {
        return 20;
    }

    signal_byte = 'r';
    if (write(ready_fd, &signal_byte, 1) != 1) {
        (void) offset_store_close(store);
        return 24;
    }

    if (read(start_fd, &signal_byte, 1) != 1) {
        (void) offset_store_close(store);
        return 25;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        const char *root_name;
        OffsetStoreStatus status;

        root_name = stress_root_name_for(iteration);
        status = offset_store_set_root(store, root_name, object);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(store);
            return 21;
        }

        if ((iteration % 3u) == 0u) {
            status = offset_store_remove_root(store, root_name);
            if (status != OFFSET_STORE_STATUS_OK) {
                (void) offset_store_close(store);
                return 22;
            }
        }
    }

    if (offset_store_close(store) != OFFSET_STORE_STATUS_OK) {
        return 23;
    }

    return 0;
}

/**
 * @brief Runs one index-writer stress worker process.
 *
 * @param store Pre-opened store descriptor inherited by the worker process.
 * @param object Object handle to publish under rotating index keys.
 * @param iterations Number of update iterations to execute.
 * @param ready_fd Pipe descriptor used to signal that attach completed.
 * @param start_fd Pipe descriptor used to wait for the parent start signal.
 * @return Process exit code for the worker.
 */
static int run_index_writer_stress_worker(
    OffsetStore *store,
    OffsetPtr object,
    unsigned int iterations,
    int ready_fd,
    int start_fd
)
{
    unsigned int iteration;
    char signal_byte;

    if (store == NULL) {
        return 30;
    }

    signal_byte = 'i';
    if (write(ready_fd, &signal_byte, 1) != 1) {
        (void) offset_store_close(store);
        return 34;
    }

    if (read(start_fd, &signal_byte, 1) != 1) {
        (void) offset_store_close(store);
        return 35;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        const char *key;
        OffsetStoreStatus status;

        key = stress_index_key_for(iteration);
        status = offset_store_index_put(store, key, object);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(store);
            return 31;
        }

        if ((iteration % 4u) == 0u) {
            status = offset_store_index_remove(store, key);
            if (status != OFFSET_STORE_STATUS_OK) {
                (void) offset_store_close(store);
                return 32;
            }
        }
    }

    if (offset_store_close(store) != OFFSET_STORE_STATUS_OK) {
        return 33;
    }

    return 0;
}

/**
 * @brief Runs one reader stress worker process for roots and index entries.
 *
 * @param store Pre-opened store descriptor inherited by the worker process.
 * @param expected_object Object handle that may appear when an entry is present.
 * @param iterations Number of lookup iterations to execute.
 * @param ready_fd Pipe descriptor used to signal that attach completed.
 * @param start_fd Pipe descriptor used to wait for the parent start signal.
 * @return Process exit code for the worker.
 */
static int run_directory_reader_stress_worker(
    OffsetStore *store,
    OffsetPtr expected_object,
    unsigned int iterations,
    int ready_fd,
    int start_fd
)
{
    unsigned int iteration;
    char signal_byte;

    if (store == NULL) {
        return 40;
    }

    signal_byte = 'd';
    if (write(ready_fd, &signal_byte, 1) != 1) {
        (void) offset_store_close(store);
        return 47;
    }

    if (read(start_fd, &signal_byte, 1) != 1) {
        (void) offset_store_close(store);
        return 48;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        const char *root_name;
        const char *index_key;
        OffsetPtr resolved_object;
        bool contains;
        OffsetStoreStatus status;

        root_name = stress_root_name_for(iteration);
        index_key = stress_index_key_for(iteration);

        status = offset_store_get_root(store, root_name, &resolved_object);
        if (status == OFFSET_STORE_STATUS_OK) {
            if (resolved_object.offset != expected_object.offset) {
                (void) offset_store_close(store);
                return 41;
            }
        } else if (status != OFFSET_STORE_STATUS_NOT_FOUND) {
            (void) offset_store_close(store);
            return 42;
        }

        status = offset_store_index_contains(store, index_key, &contains);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(store);
            return 43;
        }

        status = offset_store_index_get(store, index_key, &resolved_object);
        if (status == OFFSET_STORE_STATUS_OK) {
            if (resolved_object.offset != expected_object.offset) {
                (void) offset_store_close(store);
                return 44;
            }
        } else if (status != OFFSET_STORE_STATUS_NOT_FOUND) {
            (void) offset_store_close(store);
            return 45;
        }

        /*
         * `offset_store_index_contains(...)` and `offset_store_index_get(...)`
         * are separate read-side operations. Writers may remove or reinsert an
         * entry between those calls, so `contains == true` does not guarantee a
         * later `get(...)` will still succeed.
         */
        (void) contains;
    }

    if (offset_store_close(store) != OFFSET_STORE_STATUS_OK) {
        return 46;
    }

    return 0;
}

/** @} */

/**
 * @name Deterministic Test Cases
 * @{
 */


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
 * @brief Verifies that root and index publication are independent from the allocator lock.
 */
static void test_root_and_index_updates_do_not_block_on_allocator_lock(void)
{
    char name[64];
    int pipe_fds[2];
    pid_t child_pid;
    OffsetStore creator;
    OffsetPtr stored_object;
    struct pollfd poll_fd;
    char signal_buffer[2];
    int status;

    make_region_name(name, sizeof(name), "store-subsystem-locks");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(0, pipe(pipe_fds));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 8192));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&creator.region, 17, 32, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_allocator_lock(&creator.region));

    child_pid = fork();
    TEST_ASSERT_TRUE(child_pid >= 0);
    if (child_pid == 0) {
        OffsetStore attached;

        close(pipe_fds[0]);
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            offset_store_set_root(&attached, "shared-root", stored_object)
        );
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            offset_store_index_put(&attached, "shared-index", stored_object)
        );
        signal_buffer[0] = 'r';
        signal_buffer[1] = 'i';
        TEST_ASSERT_EQUAL_INT(2, write(pipe_fds[1], signal_buffer, sizeof(signal_buffer)));
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
        close(pipe_fds[1]);
        _exit(0);
    }

    close(pipe_fds[1]);
    poll_fd.fd = pipe_fds[0];
    poll_fd.events = POLLIN;

    /*
     * Root and index updates should use their own subsystem mutexes, so the
     * child should complete even while the allocator mutex is still held.
     */
    TEST_ASSERT_EQUAL_INT(1, poll(&poll_fd, 1, 1000));
    TEST_ASSERT_EQUAL_INT(2, read(pipe_fds[0], signal_buffer, sizeof(signal_buffer)));
    TEST_ASSERT_EQUAL_CHAR('r', signal_buffer[0]);
    TEST_ASSERT_EQUAL_CHAR('i', signal_buffer[1]);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_allocator_unlock(&creator.region));
    TEST_ASSERT_EQUAL_INT(child_pid, waitpid(child_pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    close(pipe_fds[0]);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies roots and index operations remain consistent under reader-writer contention.
 */
static void test_roots_and_index_reader_writer_contention_stress(void)
{
    enum {
        root_writer_iterations = 400,
        index_writer_iterations = 400,
        reader_iterations = 800,
        reader_count = 3,
        worker_count = 2 + reader_count
    };

    char name[64];
    OffsetStore store;
    OffsetStore worker_stores[worker_count];
    OffsetPtr shared_object;
    pid_t child_pids[worker_count];
    int ready_pipes[worker_count][2];
    int start_pipes[worker_count][2];
    size_t child_index;
    char signal_byte;

    make_region_name(name, sizeof(name), "store-directory-stress");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 16384));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 18, 64, &shared_object)
    );

    for (child_index = 0; child_index < worker_count; ++child_index) {
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            offset_store_open_existing(&worker_stores[child_index], name)
        );
    }

    for (child_index = 0; child_index < worker_count; ++child_index) {
        TEST_ASSERT_EQUAL_INT(0, pipe(ready_pipes[child_index]));
        TEST_ASSERT_EQUAL_INT(0, pipe(start_pipes[child_index]));
    }

    child_pids[0] = fork();
    TEST_ASSERT_TRUE(child_pids[0] >= 0);
    if (child_pids[0] == 0) {
        close(ready_pipes[0][0]);
        close(start_pipes[0][1]);
        _exit(
            run_root_writer_stress_worker(
                &worker_stores[0],
                shared_object,
                root_writer_iterations,
                ready_pipes[0][1],
                start_pipes[0][0]
            )
        );
    }
    close(ready_pipes[0][1]);
    close(start_pipes[0][0]);

    child_pids[1] = fork();
    TEST_ASSERT_TRUE(child_pids[1] >= 0);
    if (child_pids[1] == 0) {
        close(ready_pipes[1][0]);
        close(start_pipes[1][1]);
        _exit(
            run_index_writer_stress_worker(
                &worker_stores[1],
                shared_object,
                index_writer_iterations,
                ready_pipes[1][1],
                start_pipes[1][0]
            )
        );
    }
    close(ready_pipes[1][1]);
    close(start_pipes[1][0]);

    for (child_index = 0; child_index < reader_count; ++child_index) {
        child_pids[2 + child_index] = fork();
        TEST_ASSERT_TRUE(child_pids[2 + child_index] >= 0);
        if (child_pids[2 + child_index] == 0) {
            close(ready_pipes[2 + child_index][0]);
            close(start_pipes[2 + child_index][1]);
            _exit(
                run_directory_reader_stress_worker(
                    &worker_stores[2 + child_index],
                    shared_object,
                    reader_iterations,
                    ready_pipes[2 + child_index][1],
                    start_pipes[2 + child_index][0]
                )
            );
        }

        close(ready_pipes[2 + child_index][1]);
        close(start_pipes[2 + child_index][0]);
    }

    for (child_index = 0; child_index < worker_count; ++child_index) {
        TEST_ASSERT_EQUAL_INT(1, read(ready_pipes[child_index][0], &signal_byte, 1));
        close(ready_pipes[child_index][0]);
    }

    for (child_index = 0; child_index < worker_count; ++child_index) {
        signal_byte = 's';
        TEST_ASSERT_EQUAL_INT(1, write(start_pipes[child_index][1], &signal_byte, 1));
        close(start_pipes[child_index][1]);
    }

    for (child_index = 0; child_index < sizeof(child_pids) / sizeof(child_pids[0]); ++child_index) {
        int status;
        pid_t waited_pid;

        waited_pid = waitpid(child_pids[child_index], &status, 0);
        TEST_ASSERT_EQUAL_INT(child_pids[child_index], waited_pid);
        TEST_ASSERT_TRUE(WIFEXITED(status));
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0,
            WEXITSTATUS(status),
            "roots/index contention stress worker exited with failure"
        );
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_validate(&store));

    for (child_index = 0; child_index < worker_count; ++child_index) {
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&worker_stores[child_index]));
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/** @} */

/**
 * @name Test Runner
 * @{
 */

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
    RUN_TEST(test_root_and_index_updates_do_not_block_on_allocator_lock);
    RUN_TEST(test_roots_and_index_reader_writer_contention_stress);
    return UNITY_END();
}

/** @} */
