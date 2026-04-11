#define _POSIX_C_SOURCE 200809L

/**
 * @file producer.c
 * @brief Producer demonstrating all offset-store container types.
 *
 * This file serves as a syntax reference for creating shared-memory objects
 * and containers. Run with no args to see usage.
 *
 * Containers demonstrated:
 * - Basic object allocation
 * - Dynamic array (dynarray)
 * - Linked list (dynlist)
 * - Hash table (hashtable)
 * - Ring buffer (ringbuf)
 *
 * Each container type is published via the root table using a unique name.
 */

#include "offset_store/offset_store.h"
#include "offset_store/object_store.h"
#include "offset_store/store.h"
#include "offset_store/dynarray.h"
#include "offset_store/dynlist.h"
#include "offset_store/hashtable.h"
#include "offset_store/ringbuf.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

enum {
    EXAMPLE_REGION_SIZE = 65536
};

static int produce_basic_object(OffsetStore *store, const char *root_name, const char *message);
static int produce_dynarray(OffsetStore *store, const char *root_name);
static int produce_dynlist(OffsetStore *store, const char *root_name);
static int produce_hashtable(OffsetStore *store, const char *root_name);
static int produce_ringbuf(OffsetStore *store, const char *root_name);

/**
 * @brief Creates a simple object and publishes it via root table.
 *
 * Syntax:
 *   ./producer basic <shm-name> <root-name> <message>
 */
static int produce_basic_object(OffsetStore *store, const char *root_name, const char *message)
{
    OffsetPtr object;
    size_t payload_size = strlen(message) + 1;

    if (object_store_alloc(&store->region, 1, payload_size, &object) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "object_store_alloc failed\n");
        return -1;
    }

    void *payload = NULL;
    if (!offset_ptr_try_resolve(store->region.base, store->region.size,
                               object, payload_size, &payload)) {
        object_store_free(&store->region, object);
        fprintf(stderr, "offset_ptr_try_resolve failed\n");
        return -1;
    }

    memcpy(payload, message, payload_size);

    if (offset_store_set_root(store, root_name, object) != OFFSET_STORE_STATUS_OK) {
        object_store_free(&store->region, object);
        fprintf(stderr, "offset_store_set_root failed\n");
        return -1;
    }

    printf("basic: created object at offset %llu, root='%s', value='%s'\n",
           (unsigned long long)object.offset, root_name, message);
    return 0;
}

/**
 * @brief Creates a dynamic array of integers.
 *
 * Syntax:
 *   ./producer array <shm-name> <root-name>
 *
 * Stores values: 10, 20, 30, 40, 50
 */
static int produce_dynarray(OffsetStore *store, const char *root_name)
{
    OffsetPtr array = dynarray_create(&store->region, sizeof(int));
    if (offset_ptr_is_null(array)) {
        fprintf(stderr, "dynarray_create failed\n");
        return -1;
    }

    int values[] = {10, 20, 30, 40, 50};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        if (dynarray_push(&store->region, array, &values[i]) != 0) {
            fprintf(stderr, "dynarray_push failed at %zu\n", i);
            dynarray_destroy(&store->region, array);
            return -1;
        }
    }

    if (offset_store_set_root(store, root_name, array) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_set_root failed\n");
        dynarray_destroy(&store->region, array);
        return -1;
    }

    printf("array: created at offset %llu, root='%s', length=%llu\n",
           (unsigned long long)array.offset, root_name,
           (unsigned long long)dynarray_length(&store->region, array));
    return 0;
}

/**
 * @brief Creates a linked list of strings.
 *
 * Syntax:
 *   ./producer list <shm-name> <root-name>
 *
 * Stores: "alpha", "beta", "gamma"
 */
static int produce_dynlist(OffsetStore *store, const char *root_name)
{
    OffsetPtr list = dynlist_create(&store->region, 64);  /* max 64 char strings */
    if (offset_ptr_is_null(list)) {
        fprintf(stderr, "dynlist_create failed\n");
        return -1;
    }

    const char *values[] = {"alpha", "beta", "gamma"};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        if (dynlist_push_back(&store->region, list, values[i]) != 0) {
            fprintf(stderr, "dynlist_push_back failed at %zu\n", i);
            dynlist_destroy(&store->region, list);
            return -1;
        }
    }

    if (offset_store_set_root(store, root_name, list) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_set_root failed\n");
        dynlist_destroy(&store->region, list);
        return -1;
    }

    printf("list: created at offset %llu, root='%s', length=%llu\n",
           (unsigned long long)list.offset, root_name,
           (unsigned long long)dynlist_length(&store->region, list));
    return 0;
}

/**
 * @brief Creates a hash table mapping strings to integers.
 *
 * Syntax:
 *   ./producer hash <shm-name> <root-name>
 *
 * Stores: apple=100, banana=200, cherry=300
 */
static int produce_hashtable(OffsetStore *store, const char *root_name)
{
    /* 8 buckets, 64-char max key, int values */
    OffsetPtr table = hashtable_create(&store->region, 8, 64, sizeof(int));
    if (offset_ptr_is_null(table)) {
        fprintf(stderr, "hashtable_create failed\n");
        return -1;
    }

    struct { const char *key; int value; } entries[] = {
        {"apple", 100},
        {"banana", 200},
        {"cherry", 300}
    };

    for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); i++) {
        if (hashtable_put(&store->region, table, entries[i].key, &entries[i].value) != 0) {
            fprintf(stderr, "hashtable_put failed for '%s'\n", entries[i].key);
            hashtable_destroy(&store->region, table);
            return -1;
        }
    }

    if (offset_store_set_root(store, root_name, table) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_set_root failed\n");
        hashtable_destroy(&store->region, table);
        return -1;
    }

    printf("hash: created at offset %llu, root='%s', entries=%llu\n",
           (unsigned long long)table.offset, root_name,
           (unsigned long long)hashtable_length(&store->region, table));
    return 0;
}

/**
 * @brief Creates a ring buffer (bounded queue) of integers.
 *
 * Syntax:
 *   ./producer ring <shm-name> <root-name>
 *
 * Stores: 100, 200, 300, 400, 500
 */
static int produce_ringbuf(OffsetStore *store, const char *root_name)
{
    /* capacity 8, int elements */
    OffsetPtr buf = ringbuf_create(&store->region, 8, sizeof(int));
    if (offset_ptr_is_null(buf)) {
        fprintf(stderr, "ringbuf_create failed\n");
        return -1;
    }

    int values[] = {100, 200, 300, 400, 500};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        if (ringbuf_push(&store->region, buf, &values[i]) != 0) {
            fprintf(stderr, "ringbuf_push failed at %zu\n", i);
            ringbuf_destroy(&store->region, buf);
            return -1;
        }
    }

    if (offset_store_set_root(store, root_name, buf) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_set_root failed\n");
        ringbuf_destroy(&store->region, buf);
        return -1;
    }

    printf("ring: created at offset %llu, root='%s', length=%llu/%llu\n",
           (unsigned long long)buf.offset, root_name,
           (unsigned long long)ringbuf_length(&store->region, buf),
           (unsigned long long)ringbuf_capacity(&store->region, buf));
    return 0;
}

/**
 * @brief Main entry point.
 *
 * Usage:
 *   ./producer <type> <shm-name> <root-name> [args...]
 *
 * Types:
 *   basic  <root-name> <message>      - basic object with string payload
 *   array  <root-name>                 - dynamic array of integers
 *   list   <root-name>                - linked list of strings
 *   hash   <root-name>                - hash table (string -> int)
 *   ring   <root-name>                - ring buffer of integers
 *
 * Examples:
 *   ./producer basic /tmp/myshm myobj "hello world"
 *   ./producer array /tmp/myshm myarray
 *   ./producer list  /tmp/myshm mylist
 *   ./producer hash  /tmp/myshm myhash
 *   ./producer ring  /tmp/myshm myring
 */
int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <type> <shm-name> <root-name> [args...]\n\n", argv[0]);
        fprintf(stderr, "Types:\n");
        fprintf(stderr, "  basic <root-name> <message>  - basic object\n");
        fprintf(stderr, "  array  <root-name>           - dynamic array\n");
        fprintf(stderr, "  list   <root-name>          - linked list\n");
        fprintf(stderr, "  hash   <root-name>          - hash table\n");
        fprintf(stderr, "  ring   <root-name>          - ring buffer\n");
        return 1;
    }

    const char *type = argv[1];
    const char *shm_name = argv[2];
    const char *root_name = argv[3];

    OffsetStore store;
    OffsetStoreStatus status = offset_store_bootstrap(&store, shm_name, EXAMPLE_REGION_SIZE);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_bootstrap: %s\n", offset_store_status_string(status));
        return 1;
    }

    int ret = 0;

    if (strcmp(type, "basic") == 0) {
        if (argc < 5) { fprintf(stderr, "basic requires <message>\n"); ret = 1; }
        else ret = produce_basic_object(&store, root_name, argv[4]);
    }
    else if (strcmp(type, "array") == 0) {
        ret = produce_dynarray(&store, root_name);
    }
    else if (strcmp(type, "list") == 0) {
        ret = produce_dynlist(&store, root_name);
    }
    else if (strcmp(type, "hash") == 0) {
        ret = produce_hashtable(&store, root_name);
    }
    else if (strcmp(type, "ring") == 0) {
        ret = produce_ringbuf(&store, root_name);
    }
    else {
        fprintf(stderr, "Unknown type: %s\n", type);
        ret = 1;
    }

    if (ret != 0) {
        offset_store_close(&store);
        shm_region_unlink(shm_name);
    }

    return ret;
}