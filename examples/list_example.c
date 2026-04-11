#define _POSIX_C_SOURCE 200809L

#include "offset_store/dynlist.h"
#include "offset_store/offset_store.h"
#include "offset_store/store.h"

#include <stdio.h>
#include <string.h>

enum {
    EXAMPLE_REGION_SIZE = 16384
};

/**
 * @brief Demonstrates using a linked list in shared memory.
 *
 * Creates a shared region, stores a linked list with multiple elements,
 * and publishes it via the root table for discovery by other processes.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Zero on success, non-zero on failure.
 */
int main(int argc, char **argv)
{
    const char *region_name;
    const char *root_name;
    OffsetStoreStatus status;
    OffsetStore store;
    OffsetPtr list_offset;
    const char *values[] = {"first", "second", "third"};
    size_t num_values = sizeof(values) / sizeof(values[0]);
    size_t i;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <shm-name> <root-name>\n", argv[0]);
        return 1;
    }

    region_name = argv[1];
    root_name = argv[2];

    status = offset_store_bootstrap(&store, region_name, EXAMPLE_REGION_SIZE);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_bootstrap: %s\n", offset_store_status_string(status));
        return 1;
    }

    list_offset = dynlist_create(&store.region, 64);
    if (offset_ptr_is_null(list_offset)) {
        fprintf(stderr, "dynlist_create failed\n");
        offset_store_close(&store);
        shm_region_unlink(region_name);
        return 1;
    }

    for (i = 0; i < num_values; i++) {
        if (dynlist_push_back(&store.region, list_offset, values[i]) != 0) {
            fprintf(stderr, "dynlist_push_back failed at index %zu\n", i);
            dynlist_destroy(&store.region, list_offset);
            offset_store_close(&store);
            shm_region_unlink(region_name);
            return 1;
        }
    }

    status = offset_store_set_root(&store, root_name, list_offset);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_set_root: %s\n", offset_store_status_string(status));
        dynlist_destroy(&store.region, list_offset);
        offset_store_close(&store);
        shm_region_unlink(region_name);
        return 1;
    }

    printf("region=%s root=%s list_offset=%llu length=%llu\n",
        region_name,
        root_name,
        (unsigned long long) list_offset.offset,
        (unsigned long long) dynlist_length(&store.region, list_offset));

    status = offset_store_close(&store);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_close: %s\n", offset_store_status_string(status));
        shm_region_unlink(region_name);
        return 1;
    }

    return 0;
}