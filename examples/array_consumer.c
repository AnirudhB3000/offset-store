#define _POSIX_C_SOURCE 200809L

#include "offset_store/dynarray.h"
#include "offset_store/offset_store.h"
#include "offset_store/store.h"
#include "offset_store/object_store.h"

#include <stdio.h>

enum {
    EXAMPLE_REGION_SIZE = 16384
};

/**
 * @brief Demonstrates reading a dynamic array from shared memory.
 *
 * Opens an existing region, resolves the array by root name,
 * and reads all elements from it.
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
    OffsetPtr array_offset;
    uint64_t length;
    size_t i;
    int value;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <shm-name> <root-name>\n", argv[0]);
        return 1;
    }

    region_name = argv[1];
    root_name = argv[2];

    status = offset_store_open_existing(&store, region_name);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_open_existing: %s\n", offset_store_status_string(status));
        return 1;
    }

    status = offset_store_get_root(&store, root_name, &array_offset);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_get_root: %s\n", offset_store_status_string(status));
        offset_store_close(&store);
        return 1;
    }

    length = dynarray_length(&store.region, array_offset);
    printf("array_offset=%llu length=%llu values=[",
        (unsigned long long) array_offset.offset,
        (unsigned long long) length);

    for (i = 0; i < length; i++) {
        if (dynarray_get(&store.region, array_offset, i, &value) != 0) {
            fprintf(stderr, "dynarray_get failed at index %zu\n", i);
            offset_store_close(&store);
            return 1;
        }
        printf("%d%s", value, i + 1 < length ? ", " : "");
    }

    printf("]\n");

    status = offset_store_close(&store);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_close: %s\n", offset_store_status_string(status));
        return 1;
    }

    return 0;
}