#define _POSIX_C_SOURCE 200809L

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/shm_region.h"
#include "offset_store/store.h"

#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "unity.h"

/**
 * @file test_lock_contention_stress.c
 * @brief Unity stress tests for lock contention across multiple processes.
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
 * @brief Builds a unique shared-memory name for one test.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    int written;

    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/** @} */

/**
 * @name Lock Contention Stress Tests
 * @{
 */

/**
 * @brief Worker that performs repeated allocator allocations under contention.
 *
 * @param arg Pointer to worker arguments (region name, iteration count).
 * @return Exit code.
 */
typedef struct {
    int iterations;
    int worker_id;
    OffsetPtr shared_object;
    OffsetStore *store;
} ContentionWorkerArgs;

/**
 * @brief Converts one integer worker status code into a pthread return value.
 *
 * @param status Worker status code.
 * @return Encoded thread return value.
 */
static void *contention_worker_return(int status)
{
    return (void *) (intptr_t) status;
}

/**
 * @brief Joins one contention worker and returns its integer status code.
 *
 * @param worker Thread handle to join.
 * @return Worker status code.
 */
static int join_contention_worker(pthread_t worker)
{
    void *result;

    TEST_ASSERT_EQUAL_INT(0, pthread_join(worker, &result));
    return (int) (intptr_t) result;
}

static void *run_allocator_contention_worker(void *arg)
{
    ContentionWorkerArgs *args = (ContentionWorkerArgs *) arg;
    size_t i;
    OffsetStore *store;

    store = args->store;
    if (store == NULL) {
        return contention_worker_return(1);
    }

    for (i = 0; i < (size_t) args->iterations; i++) {
        void *ptr;
        if (allocator_alloc(&store->region, 64, 8, &ptr) == OFFSET_STORE_STATUS_OK) {
            allocator_free(&store->region, ptr);
        }
    }

    return contention_worker_return(0);
}

/**
 * @brief Tests allocator lock contention with multiple processes.
 */
static void test_allocator_lock_contention_multiple_processes(void)
{
    char name[64];
    OffsetStore store;
    pthread_t workers[4];
    ContentionWorkerArgs worker_args[4];
    int i;

    make_region_name(name, sizeof(name), "alloc-contention");

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_bootstrap(&store, name, 64 * 1024));

    for (i = 0; i < 4; i++) {
        worker_args[i].iterations = 50;
        worker_args[i].worker_id = i;
        worker_args[i].shared_object = offset_ptr_null();
        worker_args[i].store = &store;

        if (pthread_create(&workers[i], NULL, run_allocator_contention_worker,
                &worker_args[i]) != 0) {
            TEST_FAIL_MESSAGE("Failed to create contention worker thread");
        }
    }

    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0,
            join_contention_worker(workers[i]),
            "allocator contention worker exited with failure");
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_validate(&store));

    offset_store_close(&store);
    shm_region_unlink(name);
}

/**
 * @brief Worker that performs repeated roots rwlock operations.
 */
static void *run_roots_contention_worker(void *arg)
{
    ContentionWorkerArgs *args = (ContentionWorkerArgs *) arg;
    int i;
    OffsetPtr object;
    char root_name[32];
    OffsetStore *store;

    store = args->store;
    if (store == NULL) {
        return contention_worker_return(1);
    }

    for (i = 0; i < args->iterations; i++) {
        snprintf(root_name, sizeof(root_name), "root-%d-%d",
            args->worker_id, i % 4);

        object = args->shared_object;
        if (i % 2 == 0) {
            if (offset_store_set_root(store, root_name, object) != OFFSET_STORE_STATUS_OK) {
                return contention_worker_return(2);
            }
        } else {
            OffsetStoreStatus status = offset_store_get_root(store, root_name, &object);
            if (status != OFFSET_STORE_STATUS_OK && status != OFFSET_STORE_STATUS_NOT_FOUND) {
                return contention_worker_return(3);
            }
        }
    }

    return contention_worker_return(0);
}

/**
 * @brief Tests roots rwlock contention with multiple threads.
 */
static void test_roots_rwlock_contention_multiple_threads(void)
{
    char name[64];
    OffsetStore store;
    pthread_t workers[4];
    ContentionWorkerArgs worker_args[4];
    int i;
    OffsetPtr shared_object;

    make_region_name(name, sizeof(name), "roots-contention");

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_bootstrap(&store, name, 64 * 1024));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 1u, 32u, &shared_object)
    );

    for (i = 0; i < 4; i++) {
        worker_args[i].iterations = 50;
        worker_args[i].worker_id = i;
        worker_args[i].shared_object = shared_object;
        worker_args[i].store = &store;

        if (pthread_create(&workers[i], NULL, run_roots_contention_worker,
                &worker_args[i]) != 0) {
            TEST_FAIL_MESSAGE("Failed to create roots contention worker thread");
        }
    }

    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0,
            join_contention_worker(workers[i]),
            "roots contention worker exited with failure"
        );
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_validate(&store));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_free(&store.region, shared_object));
    offset_store_close(&store);
    shm_region_unlink(name);
}

/**
 * @brief Worker that performs repeated index operations.
 */
static void *run_index_contention_worker(void *arg)
{
    ContentionWorkerArgs *args = (ContentionWorkerArgs *) arg;
    int i;
    OffsetPtr object;
    char index_key[32];
    OffsetStore *store;

    store = args->store;
    if (store == NULL) {
        return contention_worker_return(1);
    }

    for (i = 0; i < args->iterations; i++) {
        snprintf(index_key, sizeof(index_key), "key-%d-%d",
            args->worker_id, i % 4);

        object = args->shared_object;
        if (i % 2 == 0) {
            if (offset_store_index_put(store, index_key, object) != OFFSET_STORE_STATUS_OK) {
                return contention_worker_return(2);
            }
        } else {
            OffsetStoreStatus status = offset_store_index_get(store, index_key, &object);
            if (status != OFFSET_STORE_STATUS_OK && status != OFFSET_STORE_STATUS_NOT_FOUND) {
                return contention_worker_return(3);
            }
        }
    }

    return contention_worker_return(0);
}

/**
 * @brief Tests index rwlock contention with multiple threads.
 */
static void test_index_rwlock_contention_multiple_threads(void)
{
    char name[64];
    OffsetStore store;
    pthread_t workers[4];
    ContentionWorkerArgs worker_args[4];
    int i;
    OffsetPtr shared_object;

    make_region_name(name, sizeof(name), "index-contention");

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_bootstrap(&store, name, 64 * 1024));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 2u, 32u, &shared_object)
    );

    for (i = 0; i < 4; i++) {
        worker_args[i].iterations = 50;
        worker_args[i].worker_id = i;
        worker_args[i].shared_object = shared_object;
        worker_args[i].store = &store;

        if (pthread_create(&workers[i], NULL, run_index_contention_worker,
                &worker_args[i]) != 0) {
            TEST_FAIL_MESSAGE("Failed to create index contention worker thread");
        }
    }

    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0,
            join_contention_worker(workers[i]),
            "index contention worker exited with failure"
        );
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_validate(&store));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_free(&store.region, shared_object));
    offset_store_close(&store);
    shm_region_unlink(name);
}

/**
 * @brief Worker that performs interleaved lock operations.
 */
static void *run_mixed_lock_contention_worker(void *arg)
{
    ContentionWorkerArgs *args = (ContentionWorkerArgs *) arg;
    int i;
    OffsetPtr object;
    char root_name[32];
    char index_key[32];
    OffsetStore *store;

    store = args->store;
    if (store == NULL) {
        return contention_worker_return(1);
    }

    for (i = 0; i < args->iterations; i++) {
        switch (i % 3) {
        case 0: {
            void *ptr;
            if (allocator_alloc(&store->region, 64, 8, &ptr) == OFFSET_STORE_STATUS_OK) {
                allocator_free(&store->region, ptr);
            }
            break;
        }
        case 1:
            snprintf(root_name, sizeof(root_name), "root-%d-%d",
                args->worker_id, i % 4);
            object = args->shared_object;
            if (offset_store_set_root(store, root_name, object) != OFFSET_STORE_STATUS_OK) {
                return contention_worker_return(2);
            }
            break;
        case 2:
            snprintf(index_key, sizeof(index_key), "key-%d-%d",
                args->worker_id, i % 4);
            object = args->shared_object;
            if (offset_store_index_put(store, index_key, object) != OFFSET_STORE_STATUS_OK) {
                return contention_worker_return(3);
            }
            break;
        default:
            break;
        }
    }

    return contention_worker_return(0);
}

/**
 * @brief Tests mixed lock contention across all subsystems.
 */
static void test_mixed_subsystem_lock_contention(void)
{
    char name[64];
    OffsetStore store;
    pthread_t workers[4];
    ContentionWorkerArgs worker_args[4];
    int i;
    OffsetPtr shared_object;

    make_region_name(name, sizeof(name), "mixed-lock-contention");

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_bootstrap(&store, name, 128 * 1024));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 3u, 32u, &shared_object)
    );

    for (i = 0; i < 4; i++) {
        worker_args[i].iterations = 50;
        worker_args[i].worker_id = i;
        worker_args[i].shared_object = shared_object;
        worker_args[i].store = &store;

        if (pthread_create(&workers[i], NULL, run_mixed_lock_contention_worker,
                &worker_args[i]) != 0) {
            TEST_FAIL_MESSAGE("Failed to create mixed lock contention worker");
        }
    }

    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0,
            join_contention_worker(workers[i]),
            "mixed lock contention worker exited with failure"
        );
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK,
        offset_store_validate(&store));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_free(&store.region, shared_object));
    offset_store_close(&store);
    shm_region_unlink(name);
}

/** @} */

/**
 * @name Test Runner
 * @{
 */

/**
 * @brief Runs the lock contention stress tests.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_allocator_lock_contention_multiple_processes);
    RUN_TEST(test_roots_rwlock_contention_multiple_threads);
    RUN_TEST(test_index_rwlock_contention_multiple_threads);
    RUN_TEST(test_mixed_subsystem_lock_contention);
    return UNITY_END();
}

/** @} */
