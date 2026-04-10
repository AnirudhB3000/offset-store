#include "offset_store/dynarray.h"
#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/shm_region.h"

#include "unity.h"
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

/* Unity lifecycle hooks required by the framework */
void setUp(void) {}
void tearDown(void) {}

static void make_region_name(char *buffer, size_t buffer_size, const char *suffix) {
    int written = snprintf(buffer, buffer_size, "/offset-store-dynarray-%ld-%s", (long)getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t)written < buffer_size);
}

/** Test basic push/get and length */
static void test_dynarray_basic(void) {
    char name[64];
    ShmRegion region;
    OffsetPtr arr;
    uint64_t val;
    uint64_t retrieved;

    make_region_name(name, sizeof(name), "basic");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 1u << 14));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    arr = dynarray_create(&region, sizeof(uint64_t));
    TEST_ASSERT_FALSE(offset_ptr_is_null(arr));

    val = 42;
    TEST_ASSERT_EQUAL_INT(0, dynarray_push(&region, arr, &val));
    val = 99;
    TEST_ASSERT_EQUAL_INT(0, dynarray_push(&region, arr, &val));
    TEST_ASSERT_EQUAL_UINT64(2, dynarray_length(&region, arr));

    TEST_ASSERT_EQUAL_INT(0, dynarray_get(&region, arr, 0, &retrieved));
    TEST_ASSERT_EQUAL_UINT64(42, retrieved);
    TEST_ASSERT_EQUAL_INT(0, dynarray_get(&region, arr, 1, &retrieved));
    TEST_ASSERT_EQUAL_UINT64(99, retrieved);

    dynarray_destroy(&region, arr);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/** Test concurrent push from child process */
static void test_dynarray_concurrent(void) {
    char name[64];
    ShmRegion region;
    OffsetPtr arr;
    pid_t child;
    int status;

    make_region_name(name, sizeof(name), "concurrent");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 1u << 15));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_init(&region));

    arr = dynarray_create(&region, sizeof(uint64_t));
    TEST_ASSERT_FALSE(offset_ptr_is_null(arr));

    child = fork();
    TEST_ASSERT_TRUE(child >= 0);
    if (child == 0) {
        // Child pushes 100 values
        for (uint64_t i = 0; i < 100; ++i) {
            dynarray_push(&region, arr, &i);
        }
        _exit(0);
    }
    // Parent waits for child to finish before pushing
    waitpid(child, &status, 0);
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
    // Parent pushes 100 values
    for (uint64_t i = 100; i < 200; ++i) {
        dynarray_push(&region, arr, &i);
    }

    TEST_ASSERT_EQUAL_UINT64(200, dynarray_length(&region, arr));
    // Verify a few values
    uint64_t out;
    TEST_ASSERT_EQUAL_INT(0, dynarray_get(&region, arr, 0, &out));
    TEST_ASSERT_EQUAL_UINT64(0, out);
    TEST_ASSERT_EQUAL_INT(0, dynarray_get(&region, arr, 150, &out));
    TEST_ASSERT_EQUAL_UINT64(150, out);

    dynarray_destroy(&region, arr);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_dynarray_basic);
    RUN_TEST(test_dynarray_concurrent);
    return UNITY_END();
}
