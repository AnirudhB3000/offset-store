#define _POSIX_C_SOURCE 200809L

#include "offset_store/object_store.h"
#include "offset_store/store.h"

#include <poll.h>
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
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Returns a deterministic root name for a contention reader iteration.
 *
 * @param iteration Zero-based loop iteration.
 * @return Root name string.
 */
static const char *stress_root_name_for(unsigned int iteration)
{
    static const char *names[] = {"root-a", "root-b", "root-c", "root-d"};

    return names[iteration % (sizeof(names) / sizeof(names[0]))];
}

/**
 * @brief Returns a deterministic index key for a contention reader iteration.
 *
 * @param iteration Zero-based loop iteration.
 * @return Index key string.
 */
static const char *stress_index_key_for(unsigned int iteration)
{
    static const char *keys[] = {"idx-a", "idx-b", "idx-c", "idx-d"};

    return keys[iteration % (sizeof(keys) / sizeof(keys[0]))];
}

/**
 * @brief Runs one root-writer stress worker process.
 *
 * @param region_name Shared-memory region name to open.
 * @param object Object handle to publish under rotating root names.
 * @param iterations Number of update iterations to execute.
 * @return Process exit code for the worker.
 */
static int run_root_writer_stress_worker(const char *region_name, OffsetPtr object, unsigned int iterations)
{
    OffsetStore store;
    unsigned int iteration;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 20;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        const char *root_name;
        OffsetStoreStatus status;

        root_name = stress_root_name_for(iteration);
        status = offset_store_set_root(&store, root_name, object);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 21;
        }

        if ((iteration % 3u) == 0u) {
            status = offset_store_remove_root(&store, root_name);
            if (status != OFFSET_STORE_STATUS_OK) {
                (void) offset_store_close(&store);
                return 22;
            }
        }
    }

    if (offset_store_close(&store) != OFFSET_STORE_STATUS_OK) {
        return 23;
    }

    return 0;
}

/**
 * @brief Runs one index-writer stress worker process.
 *
 * @param region_name Shared-memory region name to open.
 * @param object Object handle to publish under rotating index keys.
 * @param iterations Number of update iterations to execute.
 * @return Process exit code for the worker.
 */
static int run_index_writer_stress_worker(const char *region_name, OffsetPtr object, unsigned int iterations)
{
    OffsetStore store;
    unsigned int iteration;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 30;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        const char *key;
        OffsetStoreStatus status;

        key = stress_index_key_for(iteration);
        status = offset_store_index_put(&store, key, object);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 31;
        }

        if ((iteration % 4u) == 0u) {
            status = offset_store_index_remove(&store, key);
            if (status != OFFSET_STORE_STATUS_OK) {
                (void) offset_store_close(&store);
                return 32;
            }
        }
    }

    if (offset_store_close(&store) != OFFSET_STORE_STATUS_OK) {
        return 33;
    }

    return 0;
}

/**
 * @brief Runs one reader stress worker process for roots and index entries.
 *
 * @param region_name Shared-memory region name to open.
 * @param expected_object Object handle that may appear when an entry is present.
 * @param iterations Number of lookup iterations to execute.
 * @return Process exit code for the worker.
 */
static int run_directory_reader_stress_worker(const char *region_name, OffsetPtr expected_object, unsigned int iterations)
{
    OffsetStore store;
    unsigned int iteration;
    unsigned int successful_reads;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 40;
    }

    successful_reads = 0;

    for (iteration = 0; iteration < iterations; ++iteration) {
        const char *root_name;
        const char *index_key;
        OffsetPtr resolved_object;
        bool contains;
        OffsetStoreStatus status;

        root_name = stress_root_name_for(iteration);
        index_key = stress_index_key_for(iteration);

        status = offset_store_get_root(&store, root_name, &resolved_object);
        if (status == OFFSET_STORE_STATUS_OK) {
            if (resolved_object.offset != expected_object.offset) {
                (void) offset_store_close(&store);
                return 41;
            }
            successful_reads += 1;
        } else if (status != OFFSET_STORE_STATUS_NOT_FOUND) {
            (void) offset_store_close(&store);
            return 42;
        }

        status = offset_store_index_contains(&store, index_key, &contains);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 43;
        }

        status = offset_store_index_get(&store, index_key, &resolved_object);
        if (status == OFFSET_STORE_STATUS_OK) {
            if (resolved_object.offset != expected_object.offset) {
                (void) offset_store_close(&store);
                return 44;
            }
            successful_reads += 1;
        } else if (status != OFFSET_STORE_STATUS_NOT_FOUND) {
            (void) offset_store_close(&store);
            return 45;
        }

        /*
         * `offset_store_index_contains(...)` and `offset_store_index_get(...)`
         * are separate read-side operations. Writers may remove or reinsert an
         * entry between those calls, so `contains == true` does not guarantee a
         * later `get(...)` will still succeed.
         */
        (void) contains;
    }

    if (successful_reads == 0u) {
        (void) offset_store_close(&store);
        return 46;
    }

    if (offset_store_close(&store) != OFFSET_STORE_STATUS_OK) {
        return 47;
    }

    return 0;
}

/**
 * @brief Returns a deterministic payload size for mixed full-system stress work.
 *
 * @param iteration Zero-based loop iteration.
 * @return Payload size in bytes.
 */
static size_t mixed_stress_payload_size_for(unsigned int iteration)
{
    static const size_t sizes[] = {24u, 40u, 56u, 72u, 88u, 104u, 120u, 136u};

    return sizes[iteration % (sizeof(sizes) / sizeof(sizes[0]))];
}

/**
 * @brief Fills a shared object payload with a simple deterministic marker.
 *
 * @param store Store owning the object.
 * @param object Object handle to populate.
 * @param iteration Iteration marker to write into the payload.
 * @return Status code describing success or failure.
 */
static OffsetStoreStatus write_mixed_stress_payload(OffsetStore *store, OffsetPtr object, unsigned int iteration)
{
    ObjectHeader *header;
    unsigned char *payload;
    size_t payload_index;

    header = object_store_get_header_mut(&store->region, object);
    payload = (unsigned char *) object_store_get_payload(&store->region, object);
    if (header == NULL || payload == NULL) {
        return OFFSET_STORE_STATUS_INVALID_STATE;
    }

    for (payload_index = 0; payload_index < header->size; ++payload_index) {
        payload[payload_index] = (unsigned char) ((iteration + payload_index) & 0xffu);
    }

    return OFFSET_STORE_STATUS_OK;
}

/**
 * @brief Runs a mixed publisher/churn worker for the full-system stress test.
 *
 * @param region_name Shared-memory region name to open.
 * @param iterations Number of mixed iterations to execute.
 * @return Process exit code for the worker.
 */
static int run_mixed_publisher_stress_worker(const char *region_name, unsigned int iterations)
{
    OffsetStore store;
    OffsetPtr live_objects[4];
    unsigned int iteration;
    unsigned int slot;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 60;
    }

    for (slot = 0; slot < 4u; ++slot) {
        live_objects[slot] = offset_ptr_null();
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        OffsetPtr published_object;
        OffsetPtr transient_object;
        const char *root_name;
        const char *index_key;
        size_t payload_size;

        slot = iteration % 4u;
        root_name = stress_root_name_for(iteration);
        index_key = stress_index_key_for(iteration);
        payload_size = mixed_stress_payload_size_for(iteration);

        if (object_store_alloc(&store.region, (uint32_t) (100u + slot), payload_size, &published_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 61;
        }

        if (write_mixed_stress_payload(&store, published_object, iteration) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 62;
        }

        if (offset_store_set_root(&store, root_name, published_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 63;
        }

        if (offset_store_index_put(&store, index_key, published_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 64;
        }

        if (!offset_ptr_is_null(live_objects[slot])) {
            if (object_store_free(&store.region, live_objects[slot]) != OFFSET_STORE_STATUS_OK) {
                (void) offset_store_close(&store);
                return 65;
            }
        }

        live_objects[slot] = published_object;

        if (object_store_alloc(&store.region, 200u, 32u, &transient_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 66;
        }

        if (write_mixed_stress_payload(&store, transient_object, iteration + 1000u) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 67;
        }

        if (object_store_free(&store.region, transient_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 68;
        }
    }

    for (slot = 0; slot < 4u; ++slot) {
        if (offset_store_remove_root(&store, stress_root_name_for(slot)) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 69;
        }

        if (offset_store_index_remove(&store, stress_index_key_for(slot)) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 70;
        }

        if (!offset_ptr_is_null(live_objects[slot])) {
            if (object_store_free(&store.region, live_objects[slot]) != OFFSET_STORE_STATUS_OK) {
                (void) offset_store_close(&store);
                return 71;
            }
        }
    }

    if (offset_store_close(&store) != OFFSET_STORE_STATUS_OK) {
        return 72;
    }

    return 0;
}

/**
 * @brief Runs a mixed read-side stress worker for the full-system stress test.
 *
 * @param region_name Shared-memory region name to open.
 * @param iterations Number of lookup iterations to execute.
 * @return Process exit code for the worker.
 */
static int run_mixed_reader_stress_worker(const char *region_name, unsigned int iterations)
{
    OffsetStore store;
    unsigned int iteration;
    unsigned int successful_observations;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 80;
    }

    successful_observations = 0;

    for (iteration = 0; iteration < iterations; ++iteration) {
        const char *root_name;
        const char *index_key;
        OffsetPtr object;
        const ObjectHeader *header;
        const void *payload;
        OffsetStoreStatus status;

        root_name = stress_root_name_for(iteration);
        index_key = stress_index_key_for(iteration);

        status = offset_store_get_root(&store, root_name, &object);
        if (status == OFFSET_STORE_STATUS_OK) {
            status = object_store_validate(&store.region, object);
            if (status == OFFSET_STORE_STATUS_OK) {
                header = object_store_get_header(&store.region, object);
                payload = object_store_get_payload_const(&store.region, object);
                if (header != NULL && payload != NULL) {
                    if (header->size == 0u) {
                        (void) offset_store_close(&store);
                        return 81;
                    }
                    successful_observations += 1;
                }
            } else if (status != OFFSET_STORE_STATUS_INVALID_STATE && status != OFFSET_STORE_STATUS_NOT_FOUND) {
                (void) offset_store_close(&store);
                return 82;
            }
        } else if (status != OFFSET_STORE_STATUS_NOT_FOUND) {
            (void) offset_store_close(&store);
            return 83;
        }

        status = offset_store_index_get(&store, index_key, &object);
        if (status == OFFSET_STORE_STATUS_OK) {
            status = object_store_validate(&store.region, object);
            if (status == OFFSET_STORE_STATUS_OK) {
                header = object_store_get_header(&store.region, object);
                payload = object_store_get_payload_const(&store.region, object);
                if (header != NULL && payload != NULL) {
                    if (header->size == 0u) {
                        (void) offset_store_close(&store);
                        return 84;
                    }
                    successful_observations += 1;
                }
            } else if (status != OFFSET_STORE_STATUS_INVALID_STATE && status != OFFSET_STORE_STATUS_NOT_FOUND) {
                (void) offset_store_close(&store);
                return 85;
            }
        } else if (status != OFFSET_STORE_STATUS_NOT_FOUND) {
            (void) offset_store_close(&store);
            return 86;
        }

        /*
         * Lookup, validation, and payload access are separate operations. A
         * concurrent publisher may free or replace an object after validation
         * succeeds, so later header/payload access may legitimately return
         * `NULL`. This test only treats non-NULL accesses with invalid content
         * as failures.
         */
    }

    if (successful_observations == 0u) {
        (void) offset_store_close(&store);
        return 87;
    }

    if (offset_store_close(&store) != OFFSET_STORE_STATUS_OK) {
        return 88;
    }

    return 0;
}

/**
 * @brief Runs a validation/stats worker for the full-system stress test.
 *
 * @param region_name Shared-memory region name to open.
 * @param iterations Number of validation iterations to execute.
 * @return Process exit code for the worker.
 */
static int run_mixed_validator_stress_worker(const char *region_name, unsigned int iterations)
{
    OffsetStore store;
    unsigned int iteration;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 90;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        AllocatorStats stats;

        /*
         * `offset_store_validate(...)` currently includes allocator structure
         * validation, which is a quiescent integrity check rather than a
         * lock-stable live snapshot under concurrent allocator mutation.
         * During the active stress phase, restrict validation to region-header
         * checks plus allocator stats that are designed as snapshot reads.
         */
        if (shm_region_validate(&store.region) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 91;
        }

        if (allocator_get_stats(&store.region, &stats) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 92;
        }

        if (stats.heap_size != stats.free_bytes + stats.used_bytes) {
            (void) offset_store_close(&store);
            return 93;
        }
    }

    if (offset_store_close(&store) != OFFSET_STORE_STATUS_OK) {
        return 94;
    }

    return 0;
}

/**
 * @brief Verifies that bootstrap creates a store with valid allocator state.
 */
static void test_bootstrap_initializes_allocator_state(void)
{
    char name[64];
    OffsetStore store;

    make_region_name(name, sizeof(name), "store-bootstrap");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&store.region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
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
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&first, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_ALREADY_EXISTS, offset_store_bootstrap(&second, name, 4096));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&first));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
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
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_alloc(&creator.region, 64, 16, &allocation));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_validate(&attached.region));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
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
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_create(&region, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, offset_store_open_existing(&attached, name));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_close(&region));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies whole-store validation covers the region header and allocator state.
 */
static void test_store_validate_checks_region_and_allocator(void)
{
    char name[64];
    OffsetStore store;
    uint64_t *magic;

    make_region_name(name, sizeof(name), "store-validate");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_validate(&store));

    magic = (uint64_t *) store.region.base;
    TEST_ASSERT_NOT_NULL(magic);
    *magic = 0;
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_INVALID_STATE, offset_store_validate(&store));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that a named root can be stored, resolved, and removed.
 */
static void test_named_roots_round_trip(void)
{
    char name[64];
    OffsetStore store;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;

    make_region_name(name, sizeof(name), "store-roots-roundtrip");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 7, 32, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&store, "message", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_get_root(&store, "message", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_remove_root(&store, "message"));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_NOT_FOUND,
        offset_store_get_root(&store, "message", &resolved_object)
    );

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that named roots are visible to later attached processes.
 */
static void test_open_existing_observes_named_roots(void)
{
    char name[64];
    OffsetStore creator;
    OffsetStore attached;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;

    make_region_name(name, sizeof(name), "store-roots-attach");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&creator.region, 9, 48, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&creator, "shared", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_get_root(&attached, "shared", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that root creation replaces an existing name and validates inputs.
 */
static void test_named_roots_replace_existing_binding_and_validate_input(void)
{
    char name[64];
    char too_long_name[OFFSET_STORE_ROOT_NAME_LENGTH + 4];
    OffsetStore store;
    OffsetPtr first_object;
    OffsetPtr second_object;
    OffsetPtr resolved_object;

    memset(too_long_name, 'a', sizeof(too_long_name));
    too_long_name[sizeof(too_long_name) - 1] = '\0';

    make_region_name(name, sizeof(name), "store-roots-replace");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 11, 16, &first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 12, 16, &second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&store, "replaceable", first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_set_root(&store, "replaceable", second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_get_root(&store, "replaceable", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(second_object.offset, resolved_object.offset);
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_set_root(&store, too_long_name, second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_set_root(&store, "null-object", offset_ptr_null())
    );

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that indexed entries can be stored, resolved, queried, and removed.
 */
static void test_index_round_trip(void)
{
    char name[64];
    OffsetStore store;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;
    bool contains;

    make_region_name(name, sizeof(name), "store-index-roundtrip");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 13, 32, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&store, "message-1", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_contains(&store, "message-1", &contains)
    );
    TEST_ASSERT_TRUE(contains);
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_get(&store, "message-1", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_index_remove(&store, "message-1"));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_contains(&store, "message-1", &contains)
    );
    TEST_ASSERT_FALSE(contains);
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_NOT_FOUND,
        offset_store_index_get(&store, "message-1", &resolved_object)
    );

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that indexed entries are visible to later attached processes.
 */
static void test_open_existing_observes_index_entries(void)
{
    char name[64];
    OffsetStore creator;
    OffsetStore attached;
    OffsetPtr stored_object;
    OffsetPtr resolved_object;

    make_region_name(name, sizeof(name), "store-index-attach");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 4096));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&creator.region, 14, 48, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&creator, "shared-1", stored_object)
    );
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_get(&attached, "shared-1", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(stored_object.offset, resolved_object.offset);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies index replacement, capacity limits, and input validation.
 */
static void test_index_replace_capacity_and_validate_input(void)
{
    char name[64];
    char too_long_key[OFFSET_STORE_INDEX_KEY_LENGTH + 4];
    OffsetStore store;
    OffsetPtr first_object;
    OffsetPtr second_object;
    OffsetPtr temp_object;
    OffsetPtr resolved_object;
    bool contains;
    char key[32];
    int written;
    size_t index;

    memset(too_long_key, 'b', sizeof(too_long_key));
    too_long_key[sizeof(too_long_key) - 1] = '\0';

    make_region_name(name, sizeof(name), "store-index-validate");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 8192));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 15, 16, &first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 16, 16, &second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&store, "replaceable-1", first_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_put(&store, "replaceable-1", second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_get(&store, "replaceable-1", &resolved_object)
    );
    TEST_ASSERT_EQUAL_UINT64(second_object.offset, resolved_object.offset);

    for (index = 0; index < OFFSET_STORE_INDEX_CAPACITY - 1; ++index) {
        written = snprintf(key, sizeof(key), "k%zu", index);
        TEST_ASSERT_TRUE(written > 0);
        TEST_ASSERT_TRUE((size_t) written < sizeof(key));
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            object_store_alloc(&store.region, (uint32_t) (100 + index), 8, &temp_object)
        );
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            offset_store_index_put(&store, key, temp_object)
        );
    }

    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 999, 8, &temp_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OUT_OF_MEMORY,
        offset_store_index_put(&store, "overflow-key", temp_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_index_put(&store, too_long_key, second_object)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_index_put(&store, "null-object-1", offset_ptr_null())
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_INVALID_ARGUMENT,
        offset_store_index_contains(&store, "replaceable-1", NULL)
    );
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        offset_store_index_contains(&store, "missing-1", &contains)
    );
    TEST_ASSERT_FALSE(contains);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies that root and index publication are independent from the allocator lock.
 */
static void test_root_and_index_updates_do_not_block_on_allocator_lock(void)
{
    char name[64];
    int pipe_fds[2];
    pid_t child_pid;
    OffsetStore creator;
    OffsetPtr stored_object;
    struct pollfd poll_fd;
    char signal_buffer[2];
    int status;

    make_region_name(name, sizeof(name), "store-subsystem-locks");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(0, pipe(pipe_fds));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&creator, name, 8192));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&creator.region, 17, 32, &stored_object)
    );
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_allocator_lock(&creator.region));

    child_pid = fork();
    TEST_ASSERT_TRUE(child_pid >= 0);
    if (child_pid == 0) {
        OffsetStore attached;

        close(pipe_fds[0]);
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_open_existing(&attached, name));
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            offset_store_set_root(&attached, "shared-root", stored_object)
        );
        TEST_ASSERT_EQUAL_INT(
            OFFSET_STORE_STATUS_OK,
            offset_store_index_put(&attached, "shared-index", stored_object)
        );
        signal_buffer[0] = 'r';
        signal_buffer[1] = 'i';
        TEST_ASSERT_EQUAL_INT(2, write(pipe_fds[1], signal_buffer, sizeof(signal_buffer)));
        TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&attached));
        close(pipe_fds[1]);
        _exit(0);
    }

    close(pipe_fds[1]);
    poll_fd.fd = pipe_fds[0];
    poll_fd.events = POLLIN;

    /*
     * Root and index updates should use their own subsystem mutexes, so the
     * child should complete even while the allocator mutex is still held.
     */
    TEST_ASSERT_EQUAL_INT(1, poll(&poll_fd, 1, 1000));
    TEST_ASSERT_EQUAL_INT(2, read(pipe_fds[0], signal_buffer, sizeof(signal_buffer)));
    TEST_ASSERT_EQUAL_CHAR('r', signal_buffer[0]);
    TEST_ASSERT_EQUAL_CHAR('i', signal_buffer[1]);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_allocator_unlock(&creator.region));
    TEST_ASSERT_EQUAL_INT(child_pid, waitpid(child_pid, &status, 0));
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    close(pipe_fds[0]);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&creator));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies roots and index operations remain consistent under reader-writer contention.
 */
static void test_roots_and_index_reader_writer_contention_stress(void)
{
    enum {
        root_writer_iterations = 400,
        index_writer_iterations = 400,
        reader_iterations = 800,
        reader_count = 3
    };

    char name[64];
    OffsetStore store;
    OffsetPtr shared_object;
    pid_t child_pids[2 + reader_count];
    size_t child_index;

    make_region_name(name, sizeof(name), "store-directory-stress");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 16384));
    TEST_ASSERT_EQUAL_INT(
        OFFSET_STORE_STATUS_OK,
        object_store_alloc(&store.region, 18, 64, &shared_object)
    );

    child_pids[0] = fork();
    TEST_ASSERT_TRUE(child_pids[0] >= 0);
    if (child_pids[0] == 0) {
        _exit(run_root_writer_stress_worker(name, shared_object, root_writer_iterations));
    }

    child_pids[1] = fork();
    TEST_ASSERT_TRUE(child_pids[1] >= 0);
    if (child_pids[1] == 0) {
        _exit(run_index_writer_stress_worker(name, shared_object, index_writer_iterations));
    }

    for (child_index = 0; child_index < reader_count; ++child_index) {
        child_pids[2 + child_index] = fork();
        TEST_ASSERT_TRUE(child_pids[2 + child_index] >= 0);
        if (child_pids[2 + child_index] == 0) {
            _exit(run_directory_reader_stress_worker(name, shared_object, reader_iterations));
        }
    }

    for (child_index = 0; child_index < sizeof(child_pids) / sizeof(child_pids[0]); ++child_index) {
        int status;
        pid_t waited_pid;

        waited_pid = waitpid(child_pids[child_index], &status, 0);
        TEST_ASSERT_EQUAL_INT(child_pids[child_index], waited_pid);
        TEST_ASSERT_TRUE(WIFEXITED(status));
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0,
            WEXITSTATUS(status),
            "roots/index contention stress worker exited with failure"
        );
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_validate(&store));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Verifies mixed allocator, publication, reader, and validation activity under load.
 */
static void test_mixed_full_system_stress(void)
{
    enum {
        publisher_iterations = 300,
        reader_iterations = 600,
        validator_iterations = 300
    };

    char name[64];
    OffsetStore store;
    pid_t child_pids[4];
    size_t child_index;
    AllocatorStats stats;

    make_region_name(name, sizeof(name), "store-mixed-stress");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 32768));

    child_pids[0] = fork();
    TEST_ASSERT_TRUE(child_pids[0] >= 0);
    if (child_pids[0] == 0) {
        _exit(run_mixed_publisher_stress_worker(name, publisher_iterations));
    }

    child_pids[1] = fork();
    TEST_ASSERT_TRUE(child_pids[1] >= 0);
    if (child_pids[1] == 0) {
        _exit(run_mixed_reader_stress_worker(name, reader_iterations));
    }

    child_pids[2] = fork();
    TEST_ASSERT_TRUE(child_pids[2] >= 0);
    if (child_pids[2] == 0) {
        _exit(run_mixed_reader_stress_worker(name, reader_iterations));
    }

    child_pids[3] = fork();
    TEST_ASSERT_TRUE(child_pids[3] >= 0);
    if (child_pids[3] == 0) {
        _exit(run_mixed_validator_stress_worker(name, validator_iterations));
    }

    for (child_index = 0; child_index < sizeof(child_pids) / sizeof(child_pids[0]); ++child_index) {
        int status;
        pid_t waited_pid;

        waited_pid = waitpid(child_pids[child_index], &status, 0);
        TEST_ASSERT_EQUAL_INT(child_pids[child_index], waited_pid);
        TEST_ASSERT_TRUE(WIFEXITED(status));
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, WEXITSTATUS(status), "mixed stress worker exited with failure");
    }

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_validate(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, allocator_get_stats(&store.region, &stats));
    TEST_ASSERT_EQUAL_UINT64(stats.heap_size, stats.free_bytes + stats.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(0, stats.used_bytes);
    TEST_ASSERT_EQUAL_UINT64(stats.heap_size, stats.free_bytes);

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Runs the store lifecycle unit tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bootstrap_initializes_allocator_state);
    RUN_TEST(test_bootstrap_is_one_shot_per_region_name);
    RUN_TEST(test_open_existing_attaches_to_initialized_store);
    RUN_TEST(test_open_existing_rejects_region_without_allocator_state);
    RUN_TEST(test_store_validate_checks_region_and_allocator);
    RUN_TEST(test_named_roots_round_trip);
    RUN_TEST(test_open_existing_observes_named_roots);
    RUN_TEST(test_named_roots_replace_existing_binding_and_validate_input);
    RUN_TEST(test_index_round_trip);
    RUN_TEST(test_open_existing_observes_index_entries);
    RUN_TEST(test_index_replace_capacity_and_validate_input);
    RUN_TEST(test_root_and_index_updates_do_not_block_on_allocator_lock);
    RUN_TEST(test_roots_and_index_reader_writer_contention_stress);
    RUN_TEST(test_mixed_full_system_stress);
    return UNITY_END();
}
