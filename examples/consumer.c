#define _POSIX_C_SOURCE 200809L

/**
 * @file consumer.c
 * @brief Consumer demonstrating all offset-store container types.
 *
 * This file serves as a syntax reference for reading shared-memory objects
 * and containers. Run with no args to see usage.
 *
 * Containers demonstrated:
 * - Basic object retrieval
 * - Dynamic array (dynarray)
 * - Linked list (dynlist)
 * - Hash table (hashtable)
 * - Ring buffer (ringbuf)
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

static int consume_basic_object(OffsetStore *store, const char *root_name);
static int consume_dynarray(OffsetStore *store, const char *root_name);
static int consume_dynlist(OffsetStore *store, const char *root_name);
static int consume_hashtable(OffsetStore *store, const char *root_name);
static int consume_ringbuf(OffsetStore *store, const char *root_name);

/**
 * @brief Reads a basic object and prints its string payload.
 *
 * Syntax:
 *   ./consumer basic <shm-name> <root-name>
 */
static int consume_basic_object(OffsetStore *store, const char *root_name)
{
    OffsetPtr object;
    if (offset_store_get_root(store, root_name, &object) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_get_root failed\n");
        return -1;
    }

    /* Resolve object and read payload - assume first allocation is the payload */
    void *payload = NULL;
    /* We don't know exact size, but can try reasonable bounds */
    if (!offset_ptr_try_resolve(store->region.base, store->region.size,
                               object, 256, &payload)) {
        fprintf(stderr, "offset_ptr_try_resolve failed\n");
        return -1;
    }

    printf("basic: root='%s', offset=%llu, value='%s'\n",
           root_name, (unsigned long long)object.offset, (char *)payload);
    return 0;
}

/**
 * @brief Reads a dynamic array and prints all integer elements.
 *
 * Syntax:
 *   ./consumer array <shm-name> <root-name>
 */
static int consume_dynarray(OffsetStore *store, const char *root_name)
{
    OffsetPtr array;
    if (offset_store_get_root(store, root_name, &array) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_get_root failed\n");
        return -1;
    }

    uint64_t len = dynarray_length(&store->region, array);
    printf("array: root='%s', offset=%llu, length=%lu, values=[",
           root_name, (unsigned long long)array.offset, (unsigned long)len);

    for (uint64_t i = 0; i < len; i++) {
        int value;
        if (dynarray_get(&store->region, array, i, &value) != 0) {
            fprintf(stderr, "dynarray_get failed at %lu\n", (unsigned long)i);
            return -1;
        }
        printf("%d%s", value, i + 1 < len ? ", " : "");
    }
    printf("]\n");
    return 0;
}

/**
 * @brief Reads a linked list and prints all string elements.
 *
 * Syntax:
 *   ./consumer list <shm-name> <root-name>
 */
static int consume_dynlist(OffsetStore *store, const char *root_name)
{
    OffsetPtr list;
    if (offset_store_get_root(store, root_name, &list) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_get_root failed\n");
        return -1;
    }

    uint64_t len = dynlist_length(&store->region, list);
    printf("list: root='%s', offset=%llu, length=%lu, values=[",
           root_name, (unsigned long long)list.offset, (unsigned long)len);

    char buf[64];
    for (uint64_t i = 0; i < len; i++) {
        memset(buf, 0, sizeof(buf));
        if (dynlist_get(&store->region, list, i, buf) != 0) {
            fprintf(stderr, "dynlist_get failed at %lu\n", (unsigned long)i);
            return -1;
        }
        printf("\"%s\"%s", buf, i + 1 < len ? ", " : "");
    }
    printf("]\n");
    return 0;
}

/**
 * @brief Reads a hash table and prints all key-value pairs.
 *
 * Syntax:
 *   ./consumer hash <shm-name> <root-name>
 */
static int consume_hashtable(OffsetStore *store, const char *root_name)
{
    OffsetPtr table;
    if (offset_store_get_root(store, root_name, &table) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_get_root failed\n");
        return -1;
    }

    uint64_t len = hashtable_length(&store->region, table);
    printf("hash: root='%s', offset=%llu, entries=%lu\n",
           root_name, (unsigned long long)table.offset, (unsigned long)len);

    /* Known keys to retrieve */
    const char *keys[] = {"apple", "banana", "cherry"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        int value;
        if (hashtable_get(&store->region, table, keys[i], &value) == 0) {
            printf("  %s -> %d\n", keys[i], value);
        } else {
            printf("  %s -> (not found)\n", keys[i]);
        }
    }
    return 0;
}

/**
 * @brief Reads a ring buffer and prints all elements in FIFO order.
 *
 * Syntax:
 *   ./consumer ring <shm-name> <root-name>
 */
static int consume_ringbuf(OffsetStore *store, const char *root_name)
{
    OffsetPtr buf;
    if (offset_store_get_root(store, root_name, &buf) != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_get_root failed\n");
        return -1;
    }

    uint64_t len = ringbuf_length(&store->region, buf);
    uint64_t cap = ringbuf_capacity(&store->region, buf);
    printf("ring: root='%s', offset=%llu, length=%lu/%lu, values=[",
           root_name, (unsigned long long)buf.offset, (unsigned long)len, (unsigned long)cap);

    int value;
    while (ringbuf_pop(&store->region, buf, &value) == 0) {
        printf("%d%s", value, ringbuf_is_empty(&store->region, buf) ? "" : ", ");
    }
    printf("]\n");
    return 0;
}

/**
 * @brief Main entry point.
 *
 * Usage:
 *   ./consumer <type> <shm-name> <root-name>
 *
 * Types:
 *   basic  <root-name>               - read basic object
 *   array  <root-name>               - read dynamic array
 *   list   <root-name>              - read linked list
 *   hash   <root-name>              - read hash table
 *   ring   <root-name>              - read ring buffer
 *
 * Examples:
 *   ./consumer basic /tmp/myshm myobj
 *   ./consumer array /tmp/myshm myarray
 *   ./consumer list  /tmp/myshm mylist
 *   ./consumer hash  /tmp/myshm myhash
 *   ./consumer ring  /tmp/myshm myring
 */
int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <type> <shm-name> <root-name>\n\n", argv[0]);
        fprintf(stderr, "Types:\n");
        fprintf(stderr, "  basic <root-name>  - read basic object\n");
        fprintf(stderr, "  array  <root-name> - read dynamic array\n");
        fprintf(stderr, "  list   <root-name> - read linked list\n");
        fprintf(stderr, "  hash   <root-name> - read hash table\n");
        fprintf(stderr, "  ring   <root-name> - read ring buffer\n");
        return 1;
    }

    const char *type = argv[1];
    const char *shm_name = argv[2];
    const char *root_name = argv[3];

    OffsetStore store;
    OffsetStoreStatus status = offset_store_open_existing(&store, shm_name);
    if (status != OFFSET_STORE_STATUS_OK) {
        fprintf(stderr, "offset_store_open_existing: %s\n", offset_store_status_string(status));
        return 1;
    }

    int ret = 0;

    if (strcmp(type, "basic") == 0) {
        ret = consume_basic_object(&store, root_name);
    }
    else if (strcmp(type, "array") == 0) {
        ret = consume_dynarray(&store, root_name);
    }
    else if (strcmp(type, "list") == 0) {
        ret = consume_dynlist(&store, root_name);
    }
    else if (strcmp(type, "hash") == 0) {
        ret = consume_hashtable(&store, root_name);
    }
    else if (strcmp(type, "ring") == 0) {
        ret = consume_ringbuf(&store, root_name);
    }
    else {
        fprintf(stderr, "Unknown type: %s\n", type);
        ret = 1;
    }

    offset_store_close(&store);
    return ret;
}