#define _POSIX_C_SOURCE 200809L

#include "offset_store/shm_region.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

static void test_create_initializes_header(void)
{
    char name[64];
    ShmRegion region;
    const ShmRegionHeader *header;

    make_region_name(name, sizeof(name), "create");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&region, name, 4096));
    assert(region.creator);
    assert(region.base != NULL);
    assert(region.size == 4096);
    assert(shm_region_usable_size(&region) == 4096 - sizeof(ShmRegionHeader));

    header = shm_region_header(&region);
    assert(header != NULL);
    assert(header->version == OFFSET_STORE_REGION_VERSION);
    assert(header->total_size == 4096);

    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

static void test_open_observes_existing_mapping(void)
{
    char name[64];
    ShmRegion creator_region;
    ShmRegion attached_region;
    uint8_t *creator_data;
    uint8_t *attached_data;

    make_region_name(name, sizeof(name), "attach");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&creator_region, name, 4096));
    creator_data = (uint8_t *) shm_region_data(&creator_region);
    assert(creator_data != NULL);
    creator_data[0] = 0x5a;
    creator_data[1] = 0xa5;

    assert(shm_region_open(&attached_region, name));
    assert(!attached_region.creator);
    attached_data = (uint8_t *) shm_region_data(&attached_region);
    assert(attached_data != NULL);
    assert(attached_data[0] == 0x5a);
    assert(attached_data[1] == 0xa5);

    assert(shm_region_close(&attached_region));
    assert(shm_region_close(&creator_region));
    assert(shm_region_unlink(name));
}

static void test_open_rejects_invalid_header(void)
{
    char name[64];
    ShmRegion region;
    ShmRegion reopened;
    ShmRegionHeader *header;

    make_region_name(name, sizeof(name), "invalid");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&region, name, 4096));
    header = shm_region_header_mut(&region);
    assert(header != NULL);
    header->magic = 0;

    assert(!shm_region_open(&reopened, name));
    assert(errno == EINVAL);

    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

static void test_create_rejects_too_small_region(void)
{
    char name[64];
    ShmRegion region;

    make_region_name(name, sizeof(name), "small");
    assert(!shm_region_unlink(name));
    assert(!shm_region_create(&region, name, sizeof(ShmRegionHeader) - 1));
}

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
    assert(!shm_region_unlink(name));
    assert(pipe(pipe_fds) == 0);

    assert(shm_region_create(&region, name, 4096));
    data = (unsigned char *) shm_region_data(&region);
    assert(data != NULL);
    data[0] = 0;
    assert(shm_region_lock(&region));

    child_pid = fork();
    assert(child_pid >= 0);
    if (child_pid == 0) {
        ShmRegion child_region;
        unsigned char *child_data;

        close(pipe_fds[0]);
        assert(shm_region_open(&child_region, name));
        assert(shm_region_lock(&child_region));
        child_data = (unsigned char *) shm_region_data(&child_region);
        assert(child_data != NULL);
        child_data[0] = 1;
        signal_byte = 'x';
        assert(write(pipe_fds[1], &signal_byte, 1) == 1);
        assert(shm_region_unlock(&child_region));
        assert(shm_region_close(&child_region));
        close(pipe_fds[1]);
        _exit(0);
    }

    close(pipe_fds[1]);
    poll_fd.fd = pipe_fds[0];
    poll_fd.events = POLLIN;

    /* The child should remain blocked until the parent releases the lock. */
    assert(poll(&poll_fd, 1, 100) == 0);
    assert(data[0] == 0);

    assert(shm_region_unlock(&region));
    assert(poll(&poll_fd, 1, 1000) == 1);
    assert(read(pipe_fds[0], &signal_byte, 1) == 1);
    assert(signal_byte == 'x');
    assert(data[0] == 1);

    assert(waitpid(child_pid, &status, 0) == child_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    close(pipe_fds[0]);
    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

int main(void)
{
    test_create_initializes_header();
    test_open_observes_existing_mapping();
    test_open_rejects_invalid_header();
    test_create_rejects_too_small_region();
    test_process_shared_mutex_coordinates_access();
    return 0;
}
