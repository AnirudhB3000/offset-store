#include "offset_store/offset_ptr.h"

#include <stdint.h>

#include "unity.h"

/**
 * @file test_offset_ptr.c
 * @brief Unity tests for offset-pointer conversion and resolution helpers.
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
 * @name Deterministic Test Cases
 * @{
 */

/**
 * @brief Verifies that the null sentinel uses offset zero.
 */
static void test_null_offset_pointer(void)
{
    OffsetPtr ptr;

    ptr = offset_ptr_null();
    TEST_ASSERT_TRUE(offset_ptr_is_null(ptr));
    TEST_ASSERT_FALSE(offset_ptr_is_in_bounds(64, ptr, 1));
}

/**
 * @brief Verifies round-trip conversion between raw pointers and offsets.
 */
static void test_round_trip_conversion(void)
{
    uint8_t region[64] = {0};
    OffsetPtr ptr;
    void *resolved;

    TEST_ASSERT_TRUE(offset_ptr_try_from_raw(region, sizeof(region), &region[8], &ptr));
    TEST_ASSERT_EQUAL_UINT64(8, ptr.offset);
    TEST_ASSERT_TRUE(offset_ptr_try_resolve(region, sizeof(region), ptr, 4, &resolved));
    TEST_ASSERT_EQUAL_PTR(&region[8], resolved);
}

/**
 * @brief Verifies that the region base itself cannot be stored as an object offset.
 */
static void test_rejects_base_pointer_as_storable_reference(void)
{
    uint8_t region[32] = {0};
    OffsetPtr ptr;

    TEST_ASSERT_FALSE(offset_ptr_try_from_raw(region, sizeof(region), region, &ptr));
}

/**
 * @brief Verifies that pointers outside the mapping are rejected.
 */
static void test_rejects_pointer_outside_region(void)
{
    uint8_t region[32] = {0};
    uint8_t outside;
    OffsetPtr ptr;

    outside = 0;
    TEST_ASSERT_FALSE(offset_ptr_try_from_raw(region, sizeof(region), &outside, &ptr));
}

/**
 * @brief Verifies span bounds checking during offset resolution.
 */
static void test_span_validation(void)
{
    uint8_t region[32] = {0};
    OffsetPtr ptr;
    void *resolved;

    TEST_ASSERT_TRUE(offset_ptr_try_from_raw(region, sizeof(region), &region[28], &ptr));
    TEST_ASSERT_TRUE(offset_ptr_is_in_bounds(sizeof(region), ptr, 4));
    TEST_ASSERT_FALSE(offset_ptr_is_in_bounds(sizeof(region), ptr, 5));
    TEST_ASSERT_TRUE(offset_ptr_try_resolve(region, sizeof(region), ptr, 4, &resolved));
    TEST_ASSERT_FALSE(offset_ptr_try_resolve(region, sizeof(region), ptr, 5, &resolved));
}

/**
 * @brief Verifies const-qualified pointer resolution.
 */
static void test_const_resolution(void)
{
    uint8_t region[16] = {0};
    OffsetPtr ptr;
    const void *resolved;

    TEST_ASSERT_TRUE(offset_ptr_try_from_raw(region, sizeof(region), &region[4], &ptr));
    TEST_ASSERT_TRUE(offset_ptr_try_resolve_const(region, sizeof(region), ptr, 2, &resolved));
    TEST_ASSERT_EQUAL_PTR(&region[4], resolved);
}

/** @} */

/**
 * @name Test Runner
 * @{
 */

/**
 * @brief Runs the offset-pointer unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_offset_pointer);
    RUN_TEST(test_round_trip_conversion);
    RUN_TEST(test_rejects_base_pointer_as_storable_reference);
    RUN_TEST(test_rejects_pointer_outside_region);
    RUN_TEST(test_span_validation);
    RUN_TEST(test_const_resolution);
    return UNITY_END();
}

/** @} */
