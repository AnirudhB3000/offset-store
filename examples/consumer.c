#define _POSIX_C_SOURCE 200809L

#include "offset_store/offset_store.h"
#include "offset_store/object_store.h"
#include "offset_store/store.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *region_name;
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
        fprintf(stderr, "usage: %s <shm-name> <object-offset>\n", argv[0]);
        return 1;
    }

    region_name = argv[1];
    object.offset = strtoull(argv[2], NULL, 10);

    status = offset_store_open_existing(&store, region_name);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_open_existing: %s\n", offset_store_status_string(status));
        return 1;
    }

    header = object_store_header(&store.region, object);
    payload = (const char *) object_store_payload_const(&store.region, object);
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
