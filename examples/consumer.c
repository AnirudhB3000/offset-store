#define _POSIX_C_SOURCE 200809L

#include "offset_store/offset_store.h"
#include "offset_store/object_store.h"
#include "offset_store/store.h"

#include <stdio.h>

/**
 * @brief Opens a store and resolves the object offset printed by the producer.
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
    OffsetPtr object;
    const ObjectHeader *header;
    const char *payload;

    /*
     * The consumer attaches to an existing region and resolves the object by
     * using the offset printed by the producer.
     */
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

    status = offset_store_get_root(&store, root_name, &object);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_get_root: %s\n", offset_store_status_string(status));
        offset_store_close(&store);
        return 1;
    }

    header = object_store_get_header(&store.region, object);
    payload = (const char *) object_store_get_payload_const(&store.region, object);
    if (header == NULL || payload == NULL) {
        fprintf(stderr, "failed to resolve object at offset %llu\n",
            (unsigned long long) object.offset);
        offset_store_close(&store);
        return 1;
    }

    printf("type=%u size=%u payload=%s\n", header->type, header->size, payload);

    status = offset_store_close(&store);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_close: %s\n", offset_store_status_string(status));
        return 1;
    }

    return 0;
}
