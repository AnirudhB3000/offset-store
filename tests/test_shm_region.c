#define _POSIX_C_SOURCE 200809L

#include "offset_store/shm_region.h"

#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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
    assert(written > 0);
    assert((size_t) written < buffer_size);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(region.creator);
    assert(region.base != NULL);
    assert(region.size == 4096);
    assert(shm_region_header_size() > 0);
    assert(shm_region_usable_size(&region) == 4096 - shm_region_header_size());

    assert(shm_region_get_version(&region, &version) == OFFSET_STORE_STATUS_OK);
    assert(version == OFFSET_STORE_REGION_VERSION);
    assert(shm_region_get_total_size(&region, &total_size) == OFFSET_STORE_STATUS_OK);
    assert(total_size == 4096);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&creator_region, name, 4096) == OFFSET_STORE_STATUS_OK);
    creator_data = (uint8_t *) shm_region_data(&creator_region);
    assert(creator_data != NULL);
    creator_data[0] = 0x5a;
    creator_data[1] = 0xa5;

    assert(shm_region_open(&attached_region, name) == OFFSET_STORE_STATUS_OK);
    assert(!attached_region.creator);
    attached_data = (const uint8_t *) shm_region_data_const(&attached_region);
    assert(attached_data != NULL);
    assert(attached_data[0] == 0x5a);
    assert(attached_data[1] == 0xa5);

    assert(shm_region_close(&attached_region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_close(&creator_region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    /*
     * The header begins at offset zero. Corrupting the first word simulates a
     * damaged region without relying on the private header typedef.
     */
    magic = (uint64_t *) region.base;
    assert(magic != NULL);
    *magic = 0;

    assert(shm_region_open(&reopened, name) == OFFSET_STORE_STATUS_INVALID_STATE);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies that region creation rejects sizes smaller than the header.
 */
static void test_create_rejects_too_small_region(void)
{
    char name[64];
    ShmRegion region;

    make_region_name(name, sizeof(name), "small");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    assert(shm_region_create(&region, name, shm_region_header_size() - 1) == OFFSET_STORE_STATUS_INVALID_ARGUMENT);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    assert(pipe(pipe_fds) == 0);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    data = (unsigned char *) shm_region_data(&region);
    assert(data != NULL);
    data[0] = 0;
    assert(shm_region_lock(&region) == OFFSET_STORE_STATUS_OK);

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        ShmRegion child_region;
        unsigned char *child_data;

        close(pipe_fds[0]);
        assert(shm_region_open(&child_region, name) == OFFSET_STORE_STATUS_OK);
        assert(shm_region_lock(&child_region) == OFFSET_STORE_STATUS_OK);
        child_data = (unsigned char *) shm_region_data(&child_region);
        assert(child_data != NULL);
        child_data[0] = 1;
        signal_byte = 'x';
        assert(write(pipe_fds[1], &signal_byte, 1) == 1);
        assert(shm_region_unlock(&child_region) == OFFSET_STORE_STATUS_OK);
        assert(shm_region_close(&child_region) == OFFSET_STORE_STATUS_OK);
        close(pipe_fds[1]);
        _exit(0);
    }

    close(pipe_fds[1]);
    poll_fd.fd = pipe_fds[0];
    poll_fd.events = POLLIN;

    /* The child should remain blocked until the parent releases the lock. */
    assert(poll(&poll_fd, 1, 100) == 0);
    assert(data[0] == 0);

    assert(shm_region_unlock(&region) == OFFSET_STORE_STATUS_OK);
    assert(poll(&poll_fd, 1, 1000) == 1);
    assert(read(pipe_fds[0], &signal_byte, 1) == 1);
    assert(signal_byte == 'x');
    assert(data[0] == 1);

    assert(waitpid(child_pid, &status, 0) == child_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    close(pipe_fds[0]);
    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
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
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    mutable_data = shm_region_data(&region);
    const_data = shm_region_data_const(&region);
    assert(mutable_data != NULL);
    assert(const_data != NULL);
    assert(const_data == mutable_data);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Runs the shared-region unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    test_create_initializes_header();
    test_open_observes_existing_mapping();
    test_open_rejects_invalid_header();
    test_create_rejects_too_small_region();
    test_process_shared_mutex_coordinates_access();
    test_const_data_accessor();
    return 0;
}
