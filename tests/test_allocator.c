#define _POSIX_C_SOURCE 200809L

#include "offset_store/allocator.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
    assert(written > 0);
    assert((size_t) written < buffer_size);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);
    assert(allocator_validate(&region) == OFFSET_STORE_STATUS_OK);

    assert(allocator_heap_offset(&region, &heap_offset) == OFFSET_STORE_STATUS_OK);
    assert(allocator_free_list_head(&region, &free_list_head) == OFFSET_STORE_STATUS_OK);
    assert(free_list_head.offset == heap_offset);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies that allocator initialization is strict one-shot behavior.
 */
static void test_allocator_init_is_one_shot(void)
{
    char name[64];
    ShmRegion region;

    make_region_name(name, sizeof(name), "alloc-one-shot");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_ALREADY_EXISTS);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);

    assert(allocator_alloc(&region, 64, 16, &allocation) == OFFSET_STORE_STATUS_OK);
    memset(allocation, 0x5a, 64);
    assert(allocator_validate(&region) == OFFSET_STORE_STATUS_OK);

    assert(allocator_free(&region, allocation) == OFFSET_STORE_STATUS_OK);
    assert(allocator_validate(&region) == OFFSET_STORE_STATUS_OK);

    assert(allocator_alloc(&region, 64, 16, &reused) == OFFSET_STORE_STATUS_OK);
    assert(reused == allocation);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);
    assert(allocator_alloc(&region, 32, 64, &allocation) == OFFSET_STORE_STATUS_OK);
    assert(((uintptr_t) allocation % 64u) == 0);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);
    assert(allocator_alloc(&region, 48, 16, &allocation) == OFFSET_STORE_STATUS_OK);
    assert(allocator_free(&region, allocation) == OFFSET_STORE_STATUS_OK);
    assert(allocator_free(&region, allocation) == OFFSET_STORE_STATUS_INVALID_STATE);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&creator_region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&creator_region) == OFFSET_STORE_STATUS_OK);
    assert(allocator_alloc(&creator_region, 80, 16, &allocation) == OFFSET_STORE_STATUS_OK);

    assert(shm_region_open(&attached_region, name) == OFFSET_STORE_STATUS_OK);
    assert(allocator_validate(&attached_region) == OFFSET_STORE_STATUS_OK);

    assert(allocator_heap_offset(&creator_region, &creator_heap_offset) == OFFSET_STORE_STATUS_OK);
    assert(allocator_heap_offset(&attached_region, &attached_heap_offset) == OFFSET_STORE_STATUS_OK);
    assert(allocator_heap_size(&creator_region, &creator_heap_size) == OFFSET_STORE_STATUS_OK);
    assert(allocator_heap_size(&attached_region, &attached_heap_size) == OFFSET_STORE_STATUS_OK);
    assert(allocator_free_list_head(&creator_region, &creator_free_list_head) == OFFSET_STORE_STATUS_OK);
    assert(allocator_free_list_head(&attached_region, &attached_free_list_head) == OFFSET_STORE_STATUS_OK);
    assert(creator_heap_offset == attached_heap_offset);
    assert(creator_heap_size == attached_heap_size);
    assert(creator_free_list_head.offset == attached_free_list_head.offset);

    assert(shm_region_close(&attached_region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_close(&creator_region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_init(&region) == OFFSET_STORE_STATUS_OK);
    assert(allocator_alloc(&region, 32, 24, &allocation) == OFFSET_STORE_STATUS_INVALID_ARGUMENT);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Runs the allocator unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    test_allocator_init_and_validate();
    test_allocator_init_is_one_shot();
    test_allocator_alloc_and_free_round_trip();
    test_allocator_honors_alignment();
    test_allocator_rejects_double_free();
    test_allocator_state_is_visible_after_attach();
    test_allocator_rejects_invalid_alignment();
    return 0;
}
