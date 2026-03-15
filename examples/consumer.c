#define _POSIX_C_SOURCE 200809L

#include "offset_store/object_store.h"
#include "offset_store/shm_region.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *region_name;
    ShmRegion region;
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

    if (!shm_region_open(&region, region_name)) {
        perror("shm_region_open");
        return 1;
    }

    header = object_store_header(&region, object);
    payload = (const char *) object_store_payload_const(&region, object);
    if (header == NULL || payload == NULL) {
        fprintf(stderr, "failed to resolve object at offset %llu\n",
            (unsigned long long) object.offset);
        shm_region_close(&region);
        return 1;
    }

    printf("type=%u size=%u payload=%s\n", header->type, header->size, payload);

    if (!shm_region_close(&region)) {
        perror("shm_region_close");
        return 1;
    }

    return 0;
}
