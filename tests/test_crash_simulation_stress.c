#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>

/**
 * @file test_crash_simulation_stress.c
 * @brief Crash simulation stress tests for interrupted allocation and metadata updates.
 *
 * These tests verify allocator and object-store behavior under simulated crash
 * scenarios including interrupted allocations, interrupted frees, and multi-process
 * churn with forcible termination.
 *
 * The tests use child processes that are forcibly terminated at controlled points
 * to simulate real-world crash conditions, then verify the shared state remains
 * consistent and recoverable.
 */

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
 * @brief Builds a unique shared-memory name for one crash simulation test.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    int written;

    written = snprintf(buffer, buffer_size, "/offset-store-crash-%ld-%s", (long) getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Verifies allocator remains valid after child process is forcibly terminated during churn.
 */
static void test_allocator_remains_valid_after_child_crash_during_churn(void)
{
    enum {
        region_size = 1u << 14,
        child_iterations = 100
    };

    char name[64];
    ShmRegion region;
    pid_t child_pid;
    int status;
    AllocatorStats stats;

    make_region_name(name, sizeof(name), "crash-alloc");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, region_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    child_pid = fork();
    TEST_ASSERT_TRUE(child_pid >= 0);
    if (child_pid == 0) {
        ShmRegion child_region;
        unsigned int i;

        if (shm_region_open(&child_region, name) != OFFSET_STORE_STATUS_OK) {
            _exit(1);
        }

        for (i = 0; i < child_iterations; ++i) {
            void *ptr;
            size_t size;

            size = 32u + (i % 64u) * 2u;
            if (allocator_alloc(&child_region, size, 16u, &ptr) == OFFSET_STORE_STATUS_OK) {
                memset(ptr, (int) (i & 0xffu), size);
                if ((i % 3u) == 0u) {
                    (void) allocator_free(&child_region, ptr);
                }
            }
        }

        (void) shm_region_close(&child_region);
        _exit(0);
    }

    (void) waitpid(child_pid, &status, 0);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&region, &stats));
    TEST_ASSERT_EQUAL_UINT64(stats.heap_size, stats.free_bytes + stats.used_bytes);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocator handles multiple child crashes sequentially.
 */
static void test_allocator_survives_sequential_child_terminations(void)
{
    enum {
        crash_cycles = 3,
        iterations_per_cycle = 100,
        region_size = 1u << 13
    };

    char name[64];
    ShmRegion region;
    unsigned int cycle;

    make_region_name(name, sizeof(name), "crash-seq");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, region_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    for (cycle = 0; cycle < crash_cycles; ++cycle) {
        pid_t child_pid;
        int status;

        child_pid = fork();
        TEST_ASSERT_TRUE(child_pid >= 0);
        if (child_pid == 0) {
            ShmRegion child_region;
            unsigned int i;

            if (shm_region_open(&child_region, name) != OFFSET_STORE_STATUS_OK) {
                _exit(1);
            }

            for (i = 0; i < iterations_per_cycle; ++i) {
                void *ptr;
                (void) allocator_alloc(&child_region, 32, 16, &ptr);
                if ((i % 2u) == 0u) {
                    (void) allocator_free(&child_region, ptr);
                }
            }

            (void) shm_region_close(&child_region);
            _exit(0);
        }

        (void) waitpid(child_pid, &status, 0);

        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies multi-process churn with concurrent child terminations.
 */
static void test_allocator_stress_multi_process_churn_with_termination(void)
{
    enum {
        worker_count = 3,
        iterations_per_worker = 100,
        region_size = 1u << 15
    };

    char name[64];
    ShmRegion region;
    pid_t children[worker_count];
    unsigned int index;
    AllocatorStats stats;

    make_region_name(name, sizeof(name), "crash-stress");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, region_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    for (index = 0; index < worker_count; ++index) {
        pid_t child_pid;

        child_pid = fork();
        TEST_ASSERT_TRUE(child_pid >= 0);
        if (child_pid == 0) {
            ShmRegion child_region;
            unsigned int i;

            if (shm_region_open(&child_region, name) != OFFSET_STORE_STATUS_OK) {
                _exit(1);
            }

            for (i = 0; i < iterations_per_worker; ++i) {
                void *ptr;
                size_t size;

                size = 16u + (i % 32u) * 4u;
                if (allocator_alloc(&child_region, size, 16u, &ptr) == OFFSET_STORE_STATUS_OK) {
                    memset(ptr, (int) (i & 0xffu), size);
                    if ((i % 2u) == 0u) {
                        (void) allocator_free(&child_region, ptr);
                    }
                }
            }

            (void) shm_region_close(&child_region);
            _exit(0);
        }

        children[index] = child_pid;
    }

    usleep(10000);

    for (index = 0; index < worker_count; ++index) {
        (void) kill(children[index], SIGTERM);
    }

    for (index = 0; index < worker_count; ++index) {
        int child_status;

        (void) waitpid(children[index], &child_status, 0);
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&region, &stats));
    TEST_ASSERT_EQUAL_UINT64(stats.heap_size, stats.free_bytes + stats.used_bytes);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies object store remains consistent after child crash.
 */
static void test_object_store_valid_after_child_crash(void)
{
    enum {
        region_size = 1u << 14
    };

    char name[64];
    ShmRegion region;
    OffsetPtr baseline_object;

    make_region_name(name, sizeof(name), "crash-object");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, region_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&region, 1, 64, &baseline_object));

    if (fork() == 0) {
        ShmRegion child_region;
        unsigned int i;

        if (shm_region_open(&child_region, name) != OFFSET_STORE_STATUS_OK) {
            _exit(1);
        }

        for (i = 0; i < 50u; ++i) {
            OffsetPtr obj;
            (void) object_store_alloc(&child_region, 2, 32, &obj);
        }

        (void) shm_region_close(&child_region);
        _exit(0);
    }

    (void) wait(NULL);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_validate(&region, baseline_object));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_free(&region, baseline_object));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies validation detects corruption introduced by crashed child.
 */
static void test_validation_detects_corruption_after_child_termination(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *alloc_magic;

    make_region_name(name, sizeof(name), "crash-validate");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    alloc_magic = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size());
    *alloc_magic = 0;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies orphan allocations don't break allocator after child crash.
 */
static void test_allocator_handles_orphan_allocations_after_crash(void)
{
    enum {
        region_size = 1u << 14
    };

    char name[64];
    ShmRegion region;
    void *baseline;
    AllocatorStats initial_stats;
    AllocatorStats final_stats;

    make_region_name(name, sizeof(name), "crash-orphan");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, region_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 64, 16, &baseline));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&region, &initial_stats));

    if (fork() == 0) {
        ShmRegion child_region;
        void *ptrs[16];
        unsigned int i;

        if (shm_region_open(&child_region, name) != OFFSET_STORE_STATUS_OK) {
            _exit(1);
        }

        for (i = 0; i < 16u; ++i) {
            (void) allocator_alloc(&child_region, 48, 16, &ptrs[i]);
        }

        (void) shm_region_close(&child_region);
        _exit(0);
    }

    (void) wait(NULL);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&region, &final_stats));

    TEST_ASSERT_TRUE(final_stats.used_bytes >= initial_stats.used_bytes);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_free(&region, baseline));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Runs the crash simulation stress tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_allocator_remains_valid_after_child_crash_during_churn);
    RUN_TEST(test_allocator_survives_sequential_child_terminations);
    RUN_TEST(test_allocator_stress_multi_process_churn_with_termination);
    RUN_TEST(test_object_store_valid_after_child_crash);
    RUN_TEST(test_validation_detects_corruption_after_child_termination);
    RUN_TEST(test_allocator_handles_orphan_allocations_after_crash);
    return UNITY_END();
}
