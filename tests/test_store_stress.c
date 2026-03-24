#define _POSIX_C_SOURCE 200809L

#include "offset_store/object_store.h"
#include "offset_store/store.h"

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

    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    TEST_ASSERT_TRUE(written > 0);
    TEST_ASSERT_TRUE((size_t) written < buffer_size);
}

/**
 * @brief Returns a deterministic root name for one stress iteration.
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
 * @brief Returns a deterministic index key for one stress iteration.
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
 * @brief Reports worker readiness to the parent and waits for the start signal.
 *
 * @param ready_fd Write end of the shared readiness pipe.
 * @param start_fd Read end of the shared start pipe.
 * @return Process exit code on barrier failure, or zero on success.
 */
static int mixed_stress_wait_for_start_signal(int ready_fd, int start_fd)
{
    char signal_byte;

    signal_byte = 'r';
    if (write(ready_fd, &signal_byte, 1) != 1) {
        return 95;
    }

    if (read(start_fd, &signal_byte, 1) != 1) {
        return 96;
    }

    return 0;
}

/**
 * @brief Waits for one byte from a phase-control pipe.
 *
 * @param fd Read end of the phase-control pipe.
 * @return Process exit code on failure, or zero on success.
 */
static int mixed_stress_wait_for_phase_signal(int fd)
{
    char signal_byte;

    if (read(fd, &signal_byte, 1) != 1) {
        return 97;
    }

    return 0;
}

/**
 * @brief Returns a deterministic payload size for mixed stress work.
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
 * @brief Fills a shared object payload with a deterministic marker.
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
 * @param ready_fd Write end of the readiness pipe.
 * @param start_fd Read end of the start pipe.
 * @param published_fd Write end of the stable-publication signal pipe.
 * @param churn_fd Read end of the churn-phase signal pipe.
 * @return Process exit code for the worker.
 */
static int run_mixed_publisher_stress_worker(
    const char *region_name,
    unsigned int iterations,
    int ready_fd,
    int start_fd,
    int published_fd,
    int churn_fd
)
{
    OffsetStore store;
    OffsetPtr live_objects[4];
    unsigned int iteration;
    unsigned int slot;
    int barrier_status;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 60;
    }

    barrier_status = mixed_stress_wait_for_start_signal(ready_fd, start_fd);
    if (barrier_status != 0) {
        (void) offset_store_close(&store);
        return barrier_status;
    }

    for (slot = 0; slot < 4u; ++slot) {
        live_objects[slot] = offset_ptr_null();
    }

    for (slot = 0; slot < 4u; ++slot) {
        OffsetPtr published_object;
        size_t payload_size;

        payload_size = mixed_stress_payload_size_for(slot);
        if (object_store_alloc(&store.region, (uint32_t) (100u + slot), payload_size, &published_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 61;
        }

        if (write_mixed_stress_payload(&store, published_object, slot) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 62;
        }

        if (offset_store_set_root(&store, stress_root_name_for(slot), published_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 63;
        }

        if (offset_store_index_put(&store, stress_index_key_for(slot), published_object) != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 64;
        }

        live_objects[slot] = published_object;
    }

    if (write(published_fd, "p", 1) != 1) {
        (void) offset_store_close(&store);
        return 73;
    }

    barrier_status = mixed_stress_wait_for_phase_signal(churn_fd);
    if (barrier_status != 0) {
        (void) offset_store_close(&store);
        return barrier_status;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        OffsetPtr published_object;
        OffsetPtr transient_object;
        const char *root_name;
        const char *index_key;
        OffsetStoreStatus status;
        size_t payload_size;

        slot = iteration % 4u;
        root_name = stress_root_name_for(iteration);
        index_key = stress_index_key_for(iteration);
        payload_size = mixed_stress_payload_size_for(iteration);

        status = object_store_alloc(&store.region, (uint32_t) (100u + slot), payload_size, &published_object);
        if (status == OFFSET_STORE_STATUS_OUT_OF_MEMORY) {
            continue;
        }

        if (status != OFFSET_STORE_STATUS_OK) {
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

        status = object_store_alloc(&store.region, 200u, 32u, &transient_object);
        if (status == OFFSET_STORE_STATUS_OUT_OF_MEMORY) {
            continue;
        }

        if (status != OFFSET_STORE_STATUS_OK) {
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
 * @param ready_fd Write end of the readiness pipe.
 * @param start_fd Read end of the start pipe.
 * @param churn_fd Read end of the churn-phase signal pipe.
 * @return Process exit code for the worker.
 */
static int run_mixed_reader_stress_worker(
    const char *region_name,
    unsigned int iterations,
    int ready_fd,
    int start_fd,
    int churn_fd
)
{
    OffsetStore store;
    unsigned int iteration;
    unsigned int successful_observations;
    int barrier_status;
    unsigned int slot;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 80;
    }

    barrier_status = mixed_stress_wait_for_start_signal(ready_fd, start_fd);
    if (barrier_status != 0) {
        (void) offset_store_close(&store);
        return barrier_status;
    }

    successful_observations = 0;

    for (slot = 0; slot < 4u; ++slot) {
        OffsetPtr object;
        const ObjectHeader *header;
        const void *payload;
        OffsetStoreStatus status;

        status = offset_store_get_root(&store, stress_root_name_for(slot), &object);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 87;
        }

        status = object_store_validate(&store.region, object);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 87;
        }

        header = object_store_get_header(&store.region, object);
        payload = object_store_get_payload_const(&store.region, object);
        if (header == NULL || payload == NULL || header->size == 0u) {
            (void) offset_store_close(&store);
            return 87;
        }

        status = offset_store_index_get(&store, stress_index_key_for(slot), &object);
        if (status != OFFSET_STORE_STATUS_OK) {
            (void) offset_store_close(&store);
            return 87;
        }

        successful_observations += 1;
    }

    barrier_status = mixed_stress_wait_for_phase_signal(churn_fd);
    if (barrier_status != 0) {
        (void) offset_store_close(&store);
        return barrier_status;
    }

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
 * @param ready_fd Write end of the readiness pipe.
 * @param start_fd Read end of the start pipe.
 * @param churn_fd Read end of the churn-phase signal pipe.
 * @return Process exit code for the worker.
 */
static int run_mixed_validator_stress_worker(
    const char *region_name,
    unsigned int iterations,
    int ready_fd,
    int start_fd,
    int churn_fd
)
{
    OffsetStore store;
    unsigned int iteration;
    int barrier_status;

    if (offset_store_open_existing(&store, region_name) != OFFSET_STORE_STATUS_OK) {
        return 90;
    }

    barrier_status = mixed_stress_wait_for_start_signal(ready_fd, start_fd);
    if (barrier_status != 0) {
        (void) offset_store_close(&store);
        return barrier_status;
    }

    barrier_status = mixed_stress_wait_for_phase_signal(churn_fd);
    if (barrier_status != 0) {
        (void) offset_store_close(&store);
        return barrier_status;
    }

    for (iteration = 0; iteration < iterations; ++iteration) {
        AllocatorStats stats;

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
    int ready_pipe[2];
    int start_pipe[2];
    int published_pipe[2];
    int churn_pipe[2];
    OffsetStore store;
    pid_t child_pids[4];
    size_t child_index;
    AllocatorStats stats;
    char ready_signal;
    char published_signal;

    make_region_name(name, sizeof(name), "store-mixed-stress");
    TEST_ASSERT_TRUE(shm_region_unlink(name) != OFFSET_STORE_STATUS_OK);
    TEST_ASSERT_EQUAL_INT(0, pipe(ready_pipe));
    TEST_ASSERT_EQUAL_INT(0, pipe(start_pipe));
    TEST_ASSERT_EQUAL_INT(0, pipe(published_pipe));
    TEST_ASSERT_EQUAL_INT(0, pipe(churn_pipe));

    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_bootstrap(&store, name, 32768));

    child_pids[0] = fork();
    TEST_ASSERT_TRUE(child_pids[0] >= 0);
    if (child_pids[0] == 0) {
        close(ready_pipe[0]);
        close(start_pipe[1]);
        close(published_pipe[0]);
        close(churn_pipe[1]);
        _exit(
            run_mixed_publisher_stress_worker(
                name,
                publisher_iterations,
                ready_pipe[1],
                start_pipe[0],
                published_pipe[1],
                churn_pipe[0]
            )
        );
    }

    child_pids[1] = fork();
    TEST_ASSERT_TRUE(child_pids[1] >= 0);
    if (child_pids[1] == 0) {
        close(ready_pipe[0]);
        close(start_pipe[1]);
        close(churn_pipe[1]);
        close(published_pipe[0]);
        close(published_pipe[1]);
        _exit(run_mixed_reader_stress_worker(name, reader_iterations, ready_pipe[1], start_pipe[0], churn_pipe[0]));
    }

    child_pids[2] = fork();
    TEST_ASSERT_TRUE(child_pids[2] >= 0);
    if (child_pids[2] == 0) {
        close(ready_pipe[0]);
        close(start_pipe[1]);
        close(churn_pipe[1]);
        close(published_pipe[0]);
        close(published_pipe[1]);
        _exit(run_mixed_reader_stress_worker(name, reader_iterations, ready_pipe[1], start_pipe[0], churn_pipe[0]));
    }

    child_pids[3] = fork();
    TEST_ASSERT_TRUE(child_pids[3] >= 0);
    if (child_pids[3] == 0) {
        close(ready_pipe[0]);
        close(start_pipe[1]);
        close(churn_pipe[1]);
        close(published_pipe[0]);
        close(published_pipe[1]);
        _exit(run_mixed_validator_stress_worker(name, validator_iterations, ready_pipe[1], start_pipe[0], churn_pipe[0]));
    }

    close(ready_pipe[1]);
    close(start_pipe[0]);
    close(published_pipe[1]);
    close(churn_pipe[0]);

    for (child_index = 0; child_index < sizeof(child_pids) / sizeof(child_pids[0]); ++child_index) {
        TEST_ASSERT_EQUAL_INT(1, read(ready_pipe[0], &ready_signal, 1));
        TEST_ASSERT_EQUAL_CHAR('r', ready_signal);
    }

    TEST_ASSERT_EQUAL_INT(1, write(start_pipe[1], "s", 1));
    TEST_ASSERT_EQUAL_INT(1, read(published_pipe[0], &published_signal, 1));
    TEST_ASSERT_EQUAL_CHAR('p', published_signal);

    for (child_index = 1; child_index < sizeof(child_pids) / sizeof(child_pids[0]); ++child_index) {
        TEST_ASSERT_EQUAL_INT(1, write(start_pipe[1], "s", 1));
    }

    for (child_index = 0; child_index < sizeof(child_pids) / sizeof(child_pids[0]); ++child_index) {
        TEST_ASSERT_EQUAL_INT(1, write(churn_pipe[1], "c", 1));
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

    close(ready_pipe[0]);
    close(start_pipe[1]);
    close(published_pipe[0]);
    close(churn_pipe[1]);
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, offset_store_close(&store));
    TEST_ASSERT_EQUAL_INT(OFFSET_STORE_STATUS_OK, shm_region_unlink(name));
}

/**
 * @brief Runs the store stress tests.
 *
 * @return Zero on success.
 */
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mixed_full_system_stress);
    return UNITY_END();
}
