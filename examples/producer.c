#define _POSIX_C_SOURCE 200809L

#include "offset_store/offset_store.h"
#include "offset_store/object_store.h"
#include "offset_store/store.h"

#include <stdio.h>
#include <string.h>

enum {
    EXAMPLE_REGION_SIZE = 4096,
    EXAMPLE_OBJECT_TYPE = 1
};

/**
 * @brief Creates a store, writes one object payload, and prints its offset.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Zero on success, non-zero on failure.
 */
int main(int argc, char **argv)
{
    const char *region_name;
    const char *root_name;
    const char *message;
    OffsetStoreStatus status;
    OffsetStore store;
    OffsetPtr object;
    char *payload;
    size_t payload_size;

    /*
     * The producer creates a fresh region, initializes allocator state, stores
     * one object payload, and prints the resulting object offset for a consumer.
     */
    if (argc != 4) {
        fprintf(stderr, "usage: %s <shm-name> <root-name> <message>\n", argv[0]);
        return 1;
    }

    region_name = argv[1];
    root_name = argv[2];
    message = argv[3];
    payload_size = strlen(message) + 1;

    status = offset_store_bootstrap(&store, region_name, EXAMPLE_REGION_SIZE);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_bootstrap: %s\n", offset_store_status_string(status));
        return 1;
    }

    status = object_store_alloc(&store.region, EXAMPLE_OBJECT_TYPE, payload_size, &object);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "object_store_alloc: %s\n", offset_store_status_string(status));
        offset_store_close(&store);
        shm_region_unlink(region_name);
        return 1;
    }

    payload = (char *) object_store_get_payload(&store.region, object);
    if (payload == NULL) {
        fprintf(stderr, "failed to resolve payload\n");
        offset_store_close(&store);
        shm_region_unlink(region_name);
        return 1;
    }

    memcpy(payload, message, payload_size);
    status = offset_store_set_root(&store, root_name, object);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_set_root: %s\n", offset_store_status_string(status));
        offset_store_close(&store);
        shm_region_unlink(region_name);
        return 1;
    }

    printf("region=%s root=%s object_offset=%llu message=%s\n",
        region_name,
        root_name,
        (unsigned long long) object.offset,
        payload);

    status = offset_store_close(&store);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_close: %s\n", offset_store_status_string(status));
        shm_region_unlink(region_name);
        return 1;
    }

    return 0;
}
