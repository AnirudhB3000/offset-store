#define _POSIX_C_SOURCE 200809L

#include "offset_store/store.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

/**
 * @brief Builds a unique shared-memory name for one lifecycle test.
 *
 * @param buffer Destination buffer.
 * @param buffer_size Destination buffer size in bytes.
 * @param suffix Per-test suffix.
 */
static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    int written;

    /*
     * The lifecycle helpers create global POSIX shared-memory names, so tests
     * include the PID to keep runs isolated from each other.
     */
    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    assert(written > 0);
    assert((size_t) written < buffer_size);
}

/**
 * @brief Verifies that bootstrap creates a store with valid allocator state.
 */
static void test_bootstrap_initializes_allocator_state(void)
{
    char name[64];
    OffsetStore store;

    make_region_name(name, sizeof(name), "store-bootstrap");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(offset_store_bootstrap(&store, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_validate(&store.region) == OFFSET_STORE_STATUS_OK);
    assert(offset_store_close(&store) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies that bootstrap is one-shot for a given shared-memory name.
 */
static void test_bootstrap_is_one_shot_per_region_name(void)
{
    char name[64];
    OffsetStore first;
    OffsetStore second;

    make_region_name(name, sizeof(name), "store-one-shot");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(offset_store_bootstrap(&first, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(offset_store_bootstrap(&second, name, 4096) == OFFSET_STORE_STATUS_ALREADY_EXISTS);

    assert(offset_store_close(&first) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies that opening an existing store attaches to initialized state.
 */
static void test_open_existing_attaches_to_initialized_store(void)
{
    char name[64];
    OffsetStore creator;
    OffsetStore attached;
    void *allocation;

    make_region_name(name, sizeof(name), "store-open");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(offset_store_bootstrap(&creator, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(allocator_alloc(&creator.region, 64, 16, &allocation) == OFFSET_STORE_STATUS_OK);
    assert(offset_store_open_existing(&attached, name) == OFFSET_STORE_STATUS_OK);
    assert(allocator_validate(&attached.region) == OFFSET_STORE_STATUS_OK);

    assert(offset_store_close(&attached) == OFFSET_STORE_STATUS_OK);
    assert(offset_store_close(&creator) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Verifies that opening rejects a region without allocator metadata.
 */
static void test_open_existing_rejects_region_without_allocator_state(void)
{
    char name[64];
    OffsetStore attached;
    ShmRegion region;

    make_region_name(name, sizeof(name), "store-invalid");
    assert(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    assert(shm_region_create(&region, name, 4096) == OFFSET_STORE_STATUS_OK);
    assert(offset_store_open_existing(&attached, name) == OFFSET_STORE_STATUS_INVALID_STATE);

    assert(shm_region_close(&region) == OFFSET_STORE_STATUS_OK);
    assert(shm_region_unlink(name) == OFFSET_STORE_STATUS_OK);
}

/**
 * @brief Runs the store lifecycle unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    test_bootstrap_initializes_allocator_state();
    test_bootstrap_is_one_shot_per_region_name();
    test_open_existing_attaches_to_initialized_store();
    test_open_existing_rejects_region_without_allocator_state();
    return 0;
}
