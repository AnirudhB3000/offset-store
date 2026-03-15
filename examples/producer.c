#define _POSIX_C_SOURCE 200809L

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/shm_region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    EXAMPLE_REGION_SIZE = 4096,
    EXAMPLE_OBJECT_TYPE = 1
};

int main(int argc, char **argv)
{
    const char *region_name;
    const char *message;
    ShmRegion region;
    OffsetPtr object;
    char *payload;
    size_t payload_size;

    /*
     * The producer creates a fresh region, initializes allocator state, stores
     * one object payload, and prints the resulting object offset for a consumer.
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <shm-name> <message>\n", argv[0]);
        return 1;
    }

    region_name = argv[1];
    message = argv[2];
    payload_size = strlen(message) + 1;

    if (!shm_region_create(&region, region_name, EXAMPLE_REGION_SIZE)) {
        perror("shm_region_create");
        return 1;
    }

    if (!allocator_init(&region)) {
        perror("allocator_init");
        shm_region_close(&region);
        shm_region_unlink(region_name);
        return 1;
    }

    if (!object_store_alloc(&region, EXAMPLE_OBJECT_TYPE, payload_size, &object)) {
        perror("object_store_alloc");
        shm_region_close(&region);
        shm_region_unlink(region_name);
        return 1;
    }

    payload = (char *) object_store_payload(&region, object);
    if (payload == NULL) {
        fprintf(stderr, "failed to resolve payload\n");
        shm_region_close(&region);
        shm_region_unlink(region_name);
        return 1;
    }

    memcpy(payload, message, payload_size);
    printf("region=%s object_offset=%llu message=%s\n",
        region_name,
        (unsigned long long) object.offset,
        payload);

    if (!shm_region_close(&region)) {
        perror("shm_region_close");
        shm_region_unlink(region_name);
        return 1;
    }

    return 0;
}
