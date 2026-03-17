#include "offset_store/offset_store.h"

#include <stddef.h>

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
 * @brief Verifies that every public status code has a readable string.
 */
static void test_status_strings(void)
{
    TEST_ASSERT_NOT_NULL(offset_store_status_string(OFFSET_STORE_STATUS_OK));
    TEST_ASSERT_NOT_NULL(offset_store_status_string(OFFSET_STORE_STATUS_INVALID_ARGUMENT));
    TEST_ASSERT_NOT_NULL(offset_store_status_string(OFFSET_STORE_STATUS_INVALID_STATE));
    TEST_ASSERT_NOT_NULL(offset_store_status_string(OFFSET_STORE_STATUS_NOT_FOUND));
    TEST_ASSERT_NOT_NULL(offset_store_status_string(OFFSET_STORE_STATUS_ALREADY_EXISTS));
    TEST_ASSERT_NOT_NULL(offset_store_status_string(OFFSET_STORE_STATUS_OUT_OF_MEMORY));
    TEST_ASSERT_NOT_NULL(offset_store_status_string(OFFSET_STORE_STATUS_SYSTEM_ERROR));
}

/**
 * @brief Runs the status-string unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_status_strings);
    return UNITY_END();
}
