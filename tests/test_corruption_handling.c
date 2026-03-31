#define _POSIX_C_SOURCE 200809L

/**
 * @file test_corruption_handling.c
 * @brief Corruption handling tests for invalid metadata and damaged free lists.
 *
 * These tests verify that the allocator, object store, and shared region
 * correctly detect, reject, and handle various forms of data corruption
 * including:
 *
 * - Corrupted allocator header (magic, version, offsets)
 * - Corrupted free list links and structure
 * - Corrupted block headers and sizes
 * - Invalid offset pointers
 * - Damaged object headers
 * - Boundary violations
 *
 * The tests verify that validation APIs correctly identify corrupted state
 * and that accessor/mutation APIs return appropriate error codes.
 */

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"

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
 * @brief Builds a unique shared-memory name for one corruption test.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    int written;

    written = snprintf(buffer, buffer_size, "/offset-store-corrupt-%ld-%s", (long) getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Verifies that validation rejects a corrupted allocator magic value.
 */
static void test_allocator_validate_rejects_corrupted_magic(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *magic;

    make_region_name(name, sizeof(name), "corrupt-magic");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    magic = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size());
    *magic = 0xDEADBEEFDEADBEEFull;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects corrupted heap offset in allocator header.
 */
static void test_allocator_validate_rejects_corrupted_heap_offset(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *heap_offset_field;

    make_region_name(name, sizeof(name), "corrupt-heap-off");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    heap_offset_field = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size() + 16);
    *heap_offset_field = region.size + 100;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects corrupted heap size in allocator header.
 */
static void test_allocator_validate_rejects_corrupted_heap_size(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *heap_size_field;

    make_region_name(name, sizeof(name), "corrupt-heap-size");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    heap_size_field = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size() + 24);
    *heap_size_field = 0;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects misaligned heap offset.
 */
static void test_allocator_validate_rejects_misaligned_heap_offset(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *heap_offset_field;

    make_region_name(name, sizeof(name), "corrupt-heap-align");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    heap_offset_field = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size() + 16);
    *heap_offset_field = shm_region_header_size() + 1;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects a corrupted free list head pointing outside heap.
 */
static void test_allocator_validate_rejects_free_list_head_outside_heap(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *free_list_head_field;

    make_region_name(name, sizeof(name), "corrupt-freelist-head");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    free_list_head_field = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size() + 32);
    *free_list_head_field = region.size + 1;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that allocation rejects state when free list head points to invalid block.
 */
static void test_allocator_alloc_rejects_invalid_free_list_head(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *free_list_head_field;
    void *ptr;

    make_region_name(name, sizeof(name), "corrupt-alloc-freelist");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    free_list_head_field = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size() + 32);
    *free_list_head_field = region.size;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_alloc(&region, 32, 16, &ptr));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects a block with zero size.
 */
static void test_allocator_validate_rejects_zero_sized_block(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_offset;
    uint32_t *block_size;

    make_region_name(name, sizeof(name), "corrupt-zero-block");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));

    block_size = (uint32_t *) ((unsigned char *) region.base + heap_offset);
    *block_size = 0;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects a block with oversized size field.
 */
static void test_allocator_validate_rejects_oversized_block(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_offset;
    uint64_t *block_size;

    make_region_name(name, sizeof(name), "corrupt-oversized-block");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));

    block_size = (uint64_t *) ((unsigned char *) region.base + heap_offset);
    *block_size = region.size + 1;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects a block smaller than minimum size.
 */
static void test_allocator_validate_rejects_block_smaller_than_header(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_offset;
    uint64_t *block_size;

    make_region_name(name, sizeof(name), "corrupt-min-block");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));

    block_size = (uint64_t *) ((unsigned char *) region.base + heap_offset);
    *block_size = 8;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects invalid block flags.
 */
static void test_allocator_validate_rejects_invalid_block_flags(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_offset;
    uint32_t *block_flags;

    make_region_name(name, sizeof(name), "corrupt-block-flags");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));

    block_flags = (uint32_t *) ((unsigned char *) region.base + heap_offset + 16);
    *block_flags = 0xFF;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects overlapping heap blocks.
 */
static void test_allocator_validate_rejects_overlapping_blocks(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_offset;
    uint64_t *first_block_size;
    uint64_t *second_block_size;

    make_region_name(name, sizeof(name), "corrupt-overlap");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 8192));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));

    first_block_size = (uint64_t *) ((unsigned char *) region.base + heap_offset);
    *first_block_size = 512;

    second_block_size = (uint64_t *) ((unsigned char *) region.base + heap_offset + 400);
    *second_block_size = 512;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects a free list with circular references.
 */
static void test_allocator_validate_rejects_circular_free_list(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_offset;
    uint64_t heap_size;
    uint64_t *block_size;
    uint64_t *first_next_free;
    uint64_t *second_next_free;

    make_region_name(name, sizeof(name), "corrupt-circular");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 8192));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_size(&region, &heap_size));

    block_size = (uint64_t *) ((unsigned char *) region.base + heap_offset);
    *block_size = 128;

    first_next_free = (uint64_t *) ((unsigned char *) region.base + heap_offset + 24);
    *first_next_free = heap_offset + 128;

    block_size = (uint64_t *) ((unsigned char *) region.base + heap_offset + 128);
    *block_size = 128;

    second_next_free = (uint64_t *) ((unsigned char *) region.base + heap_offset + 128 + 24);
    *second_next_free = heap_offset;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that free rejects a block with corrupted allocation prefix.
 */
static void test_allocator_free_rejects_corrupted_prefix(void)
{
    char name[64];
    ShmRegion region;
    void *ptr;
    uint64_t *prefix_block_offset;

    make_region_name(name, sizeof(name), "corrupt-prefix");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 32, 16, &ptr));

    prefix_block_offset = (uint64_t *) ((unsigned char *) ptr - 8);
    *prefix_block_offset = region.size;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_free(&region, ptr));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that free rejects a block already marked as free.
 */
static void test_allocator_free_rejects_already_free_block(void)
{
    char name[64];
    ShmRegion region;
    void *ptr;
    uint32_t *block_flags;

    make_region_name(name, sizeof(name), "corrupt-double-free");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 32, 16, &ptr));

    block_flags = (uint32_t *) ((unsigned char *) ptr - 32 + 16);
    *block_flags = 1;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_free(&region, ptr));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that free rejects when payload pointer doesn't match header.
 */
static void test_allocator_free_rejects_mismatched_payload(void)
{
    char name[64];
    ShmRegion region;
    void *ptr;
    uint32_t *payload_offset;

    make_region_name(name, sizeof(name), "corrupt-mismatch");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 64, 16, &ptr));

    payload_offset = (uint32_t *) ((unsigned char *) ptr - 32 + 20);
    *payload_offset = 1;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, allocator_free(&region, ptr));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects corrupted object header.
 */
static void test_object_store_validate_rejects_corrupted_header(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr object;
    uint32_t *header_type;

    make_region_name(name, sizeof(name), "corrupt-object");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&region, 1, 32, &object));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_validate(&region, object));

    header_type = (uint32_t *) ((unsigned char *) region.base + object.offset);
    *header_type = 0xFFFFFFFF;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, object_store_validate(&region, object));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that object accessor rejects null offset.
 */
static void test_object_store_rejects_null_offset(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr null_object;

    make_region_name(name, sizeof(name), "corrupt-null");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    null_object.offset = 0;

    TEST_ASSERT_NULL(object_store_get_header(&region, null_object));
    TEST_ASSERT_NULL(object_store_get_payload(&region, null_object));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, object_store_validate(&region, null_object));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that object accessor rejects offset outside region bounds.
 */
static void test_object_store_rejects_out_of_bounds_offset(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr invalid_object;

    make_region_name(name, sizeof(name), "corrupt-oob");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    invalid_object.offset = region.size + 1;

    TEST_ASSERT_NULL(object_store_get_header(&region, invalid_object));
    TEST_ASSERT_NULL(object_store_get_payload(&region, invalid_object));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_NOT_FOUND, object_store_validate(&region, invalid_object));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects damaged region header.
 */
static void test_region_validate_rejects_corrupted_header(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *magic;

    make_region_name(name, sizeof(name), "corrupt-region");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_validate(&region));

    magic = (uint64_t *) region.base;
    *magic = 0;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, shm_region_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects corrupted region version.
 */
static void test_region_validate_rejects_corrupted_version(void)
{
    char name[64];
    ShmRegion region;
    uint32_t *version;

    make_region_name(name, sizeof(name), "corrupt-version");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_validate(&region));

    version = (uint32_t *) ((unsigned char *) region.base + 8);
    *version = 0xFFFFFFFF;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, shm_region_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that stats calculation rejects corrupted block structure.
 */
static void test_allocator_stats_rejects_corrupted_blocks(void)
{
    char name[64];
    ShmRegion region;
    uint64_t heap_offset;
    uint64_t *block_size;
    AllocatorStats stats;

    make_region_name(name, sizeof(name), "corrupt-stats");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_heap_offset(&region, &heap_offset));

    block_size = (uint64_t *) ((unsigned char *) region.base + heap_offset);
    *block_size = 0;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_get_stats(&region, &stats));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that allocation span rejects corrupted block structure.
 */
static void test_allocator_allocation_span_rejects_corrupted_block(void)
{
    char name[64];
    ShmRegion region;
    void *ptr;
    size_t span;
    uint64_t *magic;

    make_region_name(name, sizeof(name), "corrupt-span");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&region, 32, 16, &ptr));

    magic = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size());
    *magic = 0;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_get_allocation_span(&region, ptr, &span));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that freed object accessors reject freed handles.
 */
static void test_object_store_rejects_freed_object_handle(void)
{
    char name[64];
    ShmRegion region;
    OffsetPtr object;

    make_region_name(name, sizeof(name), "corrupt-freed");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_alloc(&region, 1, 32, &object));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, object_store_free(&region, object));

    TEST_ASSERT_NULL(object_store_get_header(&region, object));
    TEST_ASSERT_NULL(object_store_get_payload(&region, object));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_NOT_FOUND, object_store_validate(&region, object));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that validation rejects multiple corruption patterns in sequence.
 */
static void test_allocator_validate_rejects_multiple_corruption_patterns(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *corruption_target;

    make_region_name(name, sizeof(name), "corrupt-multi");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 8192));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    corruption_target = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size());
    *corruption_target = 0xFFFFFFFFFFFFFFFF;
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    *corruption_target = 0x4f464653414c4c43;
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&region));

    corruption_target = (uint64_t *) ((unsigned char *) region.base + shm_region_header_size() + 24);
    *corruption_target = region.size + 100;
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, allocator_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Runs the corruption handling unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_allocator_validate_rejects_corrupted_magic);
    RUN_TEST(test_allocator_validate_rejects_corrupted_heap_offset);
    RUN_TEST(test_allocator_validate_rejects_corrupted_heap_size);
    RUN_TEST(test_allocator_validate_rejects_misaligned_heap_offset);
    RUN_TEST(test_allocator_validate_rejects_free_list_head_outside_heap);
    RUN_TEST(test_allocator_alloc_rejects_invalid_free_list_head);
    RUN_TEST(test_allocator_validate_rejects_zero_sized_block);
    RUN_TEST(test_allocator_validate_rejects_oversized_block);
    RUN_TEST(test_allocator_validate_rejects_block_smaller_than_header);
    RUN_TEST(test_allocator_validate_rejects_invalid_block_flags);
    RUN_TEST(test_allocator_validate_rejects_overlapping_blocks);
    RUN_TEST(test_allocator_validate_rejects_circular_free_list);
    RUN_TEST(test_allocator_free_rejects_corrupted_prefix);
    RUN_TEST(test_allocator_free_rejects_already_free_block);
    RUN_TEST(test_allocator_free_rejects_mismatched_payload);
    RUN_TEST(test_object_store_validate_rejects_corrupted_header);
    RUN_TEST(test_object_store_rejects_null_offset);
    RUN_TEST(test_object_store_rejects_out_of_bounds_offset);
    RUN_TEST(test_region_validate_rejects_corrupted_header);
    RUN_TEST(test_region_validate_rejects_corrupted_version);
    RUN_TEST(test_allocator_stats_rejects_corrupted_blocks);
    RUN_TEST(test_allocator_allocation_span_rejects_corrupted_block);
    RUN_TEST(test_object_store_rejects_freed_object_handle);
    RUN_TEST(test_allocator_validate_rejects_multiple_corruption_patterns);
    return UNITY_END();
}
