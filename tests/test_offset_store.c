#include "offset_store/offset_store.h"

#include <assert.h>
#include <stddef.h>

/**
 * @brief Verifies that every public status code has a readable string.
 */
static void test_status_strings(void)
{
    assert(offset_store_status_string(OFFSET_STORE_STATUS_OK) != NULL);
    assert(offset_store_status_string(OFFSET_STORE_STATUS_INVALID_ARGUMENT) != NULL);
    assert(offset_store_status_string(OFFSET_STORE_STATUS_INVALID_STATE) != NULL);
    assert(offset_store_status_string(OFFSET_STORE_STATUS_NOT_FOUND) != NULL);
    assert(offset_store_status_string(OFFSET_STORE_STATUS_ALREADY_EXISTS) != NULL);
    assert(offset_store_status_string(OFFSET_STORE_STATUS_OUT_OF_MEMORY) != NULL);
    assert(offset_store_status_string(OFFSET_STORE_STATUS_SYSTEM_ERROR) != NULL);
}

/**
 * @brief Runs the status-string unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    test_status_strings();
    return 0;
}
