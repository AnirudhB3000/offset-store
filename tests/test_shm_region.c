#define _POSIX_C_SOURCE 200809L

#include "offset_store/shm_region.h"

#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
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
 * @brief Builds a unique shared-memory name for one test case.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    /*
     * POSIX shared-memory objects live in a flat namespace, so the tests use
     * the PID to avoid collisions between concurrent runs.
     */
    int written;

    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Verifies that region creation initializes shared header metadata.
 */
static void test_create_initializes_header(void)
{
    char name[64];
    ShmRegion region;
    uint32_t version;
    uint64_t total_size;

    make_region_name(name, sizeof(name), "create");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_TRUE(region.creator);
    TEST_ASSERT_NOT_NULL(region.base);
    TEST_ASSERT_EQUAL_UINT64(4096, region.size);
    TEST_ASSERT_TRUE(shm_region_header_size() > 0);
    TEST_ASSERT_EQUAL_UINT64(4096 - shm_region_header_size(), shm_region_usable_size(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_get_version(&region, &version));
    TEST_ASSERT_EQUAL_UINT32(OFFSET_STORE_REGION_VERSION, version);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_get_total_size(&region, &total_size));
    TEST_ASSERT_EQUAL_UINT64(4096, total_size);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that a second process-local descriptor can attach to the same mapping.
 */
static void test_open_observes_existing_mapping(void)
{
    char name[64];
    ShmRegion creator_region;
    ShmRegion attached_region;
    uint8_t *creator_data;
    const uint8_t *attached_data;

    make_region_name(name, sizeof(name), "attach");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&creator_region, name, 4096));
    creator_data = (uint8_t *) shm_region_data(&creator_region);
    TEST_ASSERT_NOT_NULL(creator_data);
    creator_data[0] = 0x5a;
    creator_data[1] = 0xa5;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_open(&attached_region, name));
    TEST_ASSERT_FALSE(attached_region.creator);
    attached_data = (const uint8_t *) shm_region_data_const(&attached_region);
    TEST_ASSERT_NOT_NULL(attached_data);
    TEST_ASSERT_EQUAL_UINT8(0x5a, attached_data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xa5, attached_data[1]);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&attached_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&creator_region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that attach rejects a corrupted shared header.
 */
static void test_open_rejects_invalid_header(void)
{
    char name[64];
    ShmRegion region;
    ShmRegion reopened;
    uint64_t *magic;

    make_region_name(name, sizeof(name), "invalid");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    /*
     * The header begins at offset zero. Corrupting the first word simulates a
     * damaged region without relying on the private header typedef.
     */
    magic = (uint64_t *) region.base;
    TEST_ASSERT_NOT_NULL(magic);
    *magic = 0;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, shm_region_open(&reopened, name));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies the public region validation helper on valid and corrupted headers.
 */
static void test_region_validate_reports_header_integrity(void)
{
    char name[64];
    ShmRegion region;
    uint64_t *magic;

    make_region_name(name, sizeof(name), "validate");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_validate(&region));

    magic = (uint64_t *) region.base;
    TEST_ASSERT_NOT_NULL(magic);
    *magic = 0;
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, shm_region_validate(&region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that region creation rejects sizes smaller than the header.
 */
static void test_create_rejects_too_small_region(void)
{
    char name[64];
    ShmRegion region;

    make_region_name(name, sizeof(name), "small");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        shm_region_create(&region, name, shm_region_header_size() - 1)
    );
}

/**
 * @brief Verifies that the process-shared mutex serializes access across processes.
 */
static void test_process_shared_mutex_coordinates_access(void)
{
    char name[64];
    int pipe_fds[2];
    pid_t child_pid;
    ShmRegion region;
    struct pollfd poll_fd;
    unsigned char *data;
    char signal_byte;
    int status;

    make_region_name(name, sizeof(name), "mutex");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(0, pipe(pipe_fds));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    data = (unsigned char *) shm_region_data(&region);
    TEST_ASSERT_NOT_NULL(data);
    data[0] = 0;
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_lock(&region));

    child_pid = fork();
    TEST_ASSERT_TRUE(child_pid >= 0);
    if (child_pid == 0) {
        ShmRegion child_region;
        unsigned char *child_data;

        close(pipe_fds[0]);
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_open(&child_region, name));
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_lock(&child_region));
        child_data = (unsigned char *) shm_region_data(&child_region);
        TEST_ASSERT_NOT_NULL(child_data);
        child_data[0] = 1;
        signal_byte = 'x';
        TEST_ASSERT_EQUAL_INT(1, write(pipe_fds[1], &signal_byte, 1));
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlock(&child_region));
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&child_region));
        close(pipe_fds[1]);
        _exit(0);
    }

    close(pipe_fds[1]);
    poll_fd.fd = pipe_fds[0];
    poll_fd.events = POLLIN;

    /* The child should remain blocked until the parent releases the lock. */
    TEST_ASSERT_EQUAL_INT(0, poll(&poll_fd, 1, 100));
    TEST_ASSERT_EQUAL_UINT8(0, data[0]);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlock(&region));
    TEST_ASSERT_EQUAL_INT(1, poll(&poll_fd, 1, 1000));
    TEST_ASSERT_EQUAL_INT(1, read(pipe_fds[0], &signal_byte, 1));
    TEST_ASSERT_EQUAL_CHAR('x', signal_byte);
    TEST_ASSERT_EQUAL_UINT8(1, data[0]);

    TEST_ASSERT_EQUAL_INT(child_pid, waitpid(child_pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    close(pipe_fds[0]);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that the const-qualified data accessor returns the same usable address.
 */
static void test_const_data_accessor(void)
{
    char name[64];
    ShmRegion region;
    void *mutable_data;
    const void *const_data;

    make_region_name(name, sizeof(name), "data-const");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    mutable_data = shm_region_data(&region);
    const_data = shm_region_data_const(&region);
    TEST_ASSERT_NOT_NULL(mutable_data);
    TEST_ASSERT_NOT_NULL(const_data);
    TEST_ASSERT_EQUAL_PTR(mutable_data, const_data);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies getter and accessor status contracts on invalid input.
 */
static void test_region_getters_reject_invalid_arguments(void)
{
    ShmRegion zero_region = {0};
    uint32_t version;
    uint64_t total_size;

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, shm_region_get_version(NULL, &version));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, shm_region_get_version(&zero_region, NULL));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, shm_region_get_total_size(NULL, &total_size));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_ARGUMENT, shm_region_get_total_size(&zero_region, NULL));
    TEST_ASSERT_NULL(shm_region_data(NULL));
    TEST_ASSERT_NULL(shm_region_data_const(NULL));
    TEST_ASSERT_NULL(shm_region_data(&zero_region));
    TEST_ASSERT_NULL(shm_region_data_const(&zero_region));
    TEST_ASSERT_EQUAL_UINT64(0, shm_region_usable_size(NULL));
    TEST_ASSERT_EQUAL_UINT64(0, shm_region_usable_size(&zero_region));
}

/**
 * @brief Runs the shared-region unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_initializes_header);
    RUN_TEST(test_open_observes_existing_mapping);
    RUN_TEST(test_open_rejects_invalid_header);
    RUN_TEST(test_region_validate_reports_header_integrity);
    RUN_TEST(test_create_rejects_too_small_region);
    RUN_TEST(test_process_shared_mutex_coordinates_access);
    RUN_TEST(test_const_data_accessor);
    RUN_TEST(test_region_getters_reject_invalid_arguments);
    return UNITY_END();
}
