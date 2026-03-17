#define _POSIX_C_SOURCE 200809L

#include "offset_store/allocator.h"

#include <stdint.h>
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
 * @brief Builds a unique shared-memory name for one allocator test.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    /*
     * Shared-memory names are global on the host, so tests include the PID to
     * isolate parallel or repeated runs.
     */
    int written;

    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Verifies allocator initialization and validation on a fresh region.
 */
static void test_allocator_init_and_validate(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr free_list_head;
    uint64_t heap_offset;

    make_region_name(name, sizeof(name), "alloc-init");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_free_list_head(&region, &free_list_head));
    TEST_ASSERT_EQUAL_UINT64(heap_offset, free_list_head.offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that allocator initialization is strict one-shot behavior.
 */
static void test_allocator_init_is_one_shot(void)
{
    char name[64];
    ShmRegion region;

    make_region_name(name, sizeof(name), "alloc-one-shot");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_ALREADY_EXISTS, allocator_init(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocation, free, and immediate reuse of storage.
 */
static void test_allocator_alloc_and_free_round_trip(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;
    void *reused;

    make_region_name(name, sizeof(name), "alloc-free");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 64, 16, &allocation));
    memset(allocation, 0x5a, 64);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_free(&region, allocation));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 64, 16, &reused));
    TEST_ASSERT_EQUAL_PTR(allocation, reused);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that allocations honor the requested alignment.
 */
static void test_allocator_honors_alignment(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;

    make_region_name(name, sizeof(name), "alloc-align");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 32, 64, &allocation));
    TEST_ASSERT_EQUAL_UINT64(0, (uintptr_t) allocation % 64u);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that freeing the same allocation twice is rejected.
 */
static void test_allocator_rejects_double_free(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;

    make_region_name(name, sizeof(name), "alloc-double-free");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 48, 16, &allocation));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_free(&region, allocation));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_free(&region, allocation));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocator metadata is consistent across multiple attaches.
 */
static void test_allocator_state_is_visible_after_attach(void)
{
    char name[64];
    ShmRegion creator_region;
    ShmRegion attached_region;
    void *allocation;
    OffsetPtr creator_free_list_head;
    OffsetPtr attached_free_list_head;
    uint64_t creator_heap_offset;
    uint64_t attached_heap_offset;
    uint64_t creator_heap_size;
    uint64_t attached_heap_size;

    make_region_name(name, sizeof(name), "alloc-attach");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&creator_region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&creator_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&creator_region, 80, 16, &allocation));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_open(&attached_region, name));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&attached_region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&creator_region, &creator_heap_offset));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&attached_region, &attached_heap_offset));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_size(&creator_region, &creator_heap_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_size(&attached_region, &attached_heap_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_free_list_head(&creator_region, &creator_free_list_head));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_free_list_head(&attached_region, &attached_free_list_head));
    TEST_ASSERT_EQUAL_UINT64(creator_heap_offset, attached_heap_offset);
    TEST_ASSERT_EQUAL_UINT64(creator_heap_size, attached_heap_size);
    TEST_ASSERT_EQUAL_UINT64(creator_free_list_head.offset, attached_free_list_head.offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&attached_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&creator_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocator rejection of non-power-of-two alignment requests.
 */
static void test_allocator_rejects_invalid_alignment(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;

    make_region_name(name, sizeof(name), "alloc-align-invalid");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_alloc(&region, 32, 24, &allocation));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocator getter contracts on a live allocation.
 */
static void test_allocator_getters_report_consistent_state(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;
    OffsetPtr free_list_head;
    uint64_t heap_offset;
    uint64_t heap_size;
    size_t allocation_span;

    make_region_name(name, sizeof(name), "alloc-getters");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 96, 16, &allocation));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_size(&region, &heap_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_free_list_head(&region, &free_list_head));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_allocation_span(&region, allocation, &allocation_span));
    TEST_ASSERT_TRUE(heap_offset >= shm_region_header_size());
    TEST_ASSERT_TRUE(heap_size > 0);
    TEST_ASSERT_TRUE(allocation_span >= 96);
    TEST_ASSERT_TRUE(free_list_head.offset == 0 || free_list_head.offset >= heap_offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocator statistics on a fresh heap and after allocation/free.
 */
static void test_allocator_stats_track_free_and_used_bytes(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;
    AllocatorStats initial_stats;
    AllocatorStats allocated_stats;
    AllocatorStats freed_stats;

    make_region_name(name, sizeof(name), "alloc-stats");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&region, &initial_stats));
    TEST_ASSERT_EQUAL_UINT64(initial_stats.heap_size, initial_stats.free_bytes + initial_stats.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(initial_stats.heap_size, initial_stats.free_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, initial_stats.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(1, initial_stats.free_block_count);
    TEST_ASSERT_EQUAL_UINT64(initial_stats.free_bytes, initial_stats.largest_free_block);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 96, 16, &allocation));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&region, &allocated_stats));
    TEST_ASSERT_EQUAL_UINT64(allocated_stats.heap_size, allocated_stats.free_bytes + allocated_stats.used_bytes);
    TEST_ASSERT_TRUE(allocated_stats.free_bytes < initial_stats.free_bytes);
    TEST_ASSERT_TRUE(allocated_stats.used_bytes > 0);
    TEST_ASSERT_TRUE(allocated_stats.free_block_count >= 1);
    TEST_ASSERT_TRUE(allocated_stats.largest_free_block <= allocated_stats.free_bytes);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_free(&region, allocation));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&region, &freed_stats));
    TEST_ASSERT_EQUAL_UINT64(freed_stats.heap_size, freed_stats.free_bytes + freed_stats.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, freed_stats.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(freed_stats.heap_size, freed_stats.free_bytes);
    TEST_ASSERT_TRUE(freed_stats.free_block_count >= 1);
    TEST_ASSERT_TRUE(freed_stats.largest_free_block <= freed_stats.free_bytes);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocator getter contracts on invalid input and unknown pointers.
 */
static void test_allocator_getters_reject_invalid_arguments(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_value;
    OffsetPtr head_value;
    size_t allocation_span;
    unsigned char stack_byte;

    make_region_name(name, sizeof(name), "alloc-getters-invalid");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_heap_offset(NULL, &heap_value));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_heap_size(NULL, &heap_value));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_free_list_head(NULL, &head_value));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        allocator_get_allocation_span(NULL, &stack_byte, &allocation_span)
    );

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_heap_offset(&region, NULL));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_heap_size(&region, NULL));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_free_list_head(&region, NULL));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_allocation_span(&region, NULL, &allocation_span));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_allocation_span(&region, &stack_byte, NULL));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_NOT_FOUND, allocator_get_allocation_span(&region, &stack_byte, &allocation_span));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies allocator statistics reject invalid arguments.
 */
static void test_allocator_stats_reject_invalid_arguments(void)
{
    char name[64];
    ShmRegion region;
    AllocatorStats stats;

    make_region_name(name, sizeof(name), "alloc-stats-invalid");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_stats(NULL, &stats));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_get_stats(&region, &stats));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_get_stats(&region, NULL));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Runs the allocator unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_allocator_init_and_validate);
    RUN_TEST(test_allocator_init_is_one_shot);
    RUN_TEST(test_allocator_alloc_and_free_round_trip);
    RUN_TEST(test_allocator_honors_alignment);
    RUN_TEST(test_allocator_rejects_double_free);
    RUN_TEST(test_allocator_state_is_visible_after_attach);
    RUN_TEST(test_allocator_rejects_invalid_alignment);
    RUN_TEST(test_allocator_getters_report_consistent_state);
    RUN_TEST(test_allocator_getters_reject_invalid_arguments);
    RUN_TEST(test_allocator_stats_track_free_and_used_bytes);
    RUN_TEST(test_allocator_stats_reject_invalid_arguments);
    return UNITY_END();
}
