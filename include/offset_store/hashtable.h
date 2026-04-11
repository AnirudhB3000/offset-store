#ifndef OFFSET_STORE_HASHTABLE_H
#define OFFSET_STORE_HASHTABLE_H

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/**
 * @file hashtable.h
 * @brief Offset‑based hash table stored in shared memory.
 *
 * The hash table uses chaining for collision resolution. Each bucket contains
 * a pointer to a linked list of entries. All bucket metadata is stored in the
 * shared region. A robust process‑shared mutex protects internal state.
 */

/**
 * @brief Entry stored in a hash table bucket.
 *
 * Each entry stores a key string (as an offset to a length+data block), a value
 * offset pointer, and the offset to the next entry in the bucket chain.
 */
typedef struct {
    OffsetPtr key_offset;     /**< Offset of the key string (length + data). */
    OffsetPtr value_offset;   /**< Offset of the stored value. */
    OffsetPtr next;          /**< Offset of the next entry in the chain. */
} HashTableEntry;

/**
 * @brief Header for a hash table.
 *
 * The table maintains a fixed number of buckets for O(1) average lookup. Each
 * bucket stores the offset of its entry chain head. A robust mutex provides
 * thread‑safety.
 */
typedef struct {
    uint64_t    num_buckets;  /**< Number of hash buckets. */
    size_t      key_size;     /**< Maximum key string length (excluding null). */
    size_t      val_size;    /**< Size of each value in bytes. */
    OffsetPtr   buckets;     /**< Offset of the bucket array (num_buckets * sizeof(OffsetPtr)). */
    pthread_mutex_t lock;    /**< Robust, process‑shared mutex protecting the table. */
} HashTableHeader;

/**
 * @brief Create a new hash table.
 *
 * @param region      Shared‑memory region.
 * @param num_buckets Number of hash buckets (must be non‑zero).
 * @param key_size    Maximum key string length (excluding null terminator).
 * @param val_size    Size of each value in bytes.
 * @return OffsetPtr to the table header, or a null offset on failure.
 */
OffsetPtr hashtable_create(ShmRegion *region, uint64_t num_buckets, size_t key_size, size_t val_size);

/**
 * @brief Destroy a hash table and free all entries.
 *
 * @param region         Shared‑memory region.
 * @param table_offset   Offset of the table header returned by hashtable_create.
 */
void hashtable_destroy(ShmRegion *region, OffsetPtr table_offset);

/**
 * @brief Insert or update a key‑value pair.
 *
 * @param region        Shared‑memory region.
 * @param table_offset  Offset of the table header.
 * @param key           Null‑terminated key string.
 * @param value         Pointer to the value data to store.
 * @return 0 on success, non‑zero on failure (allocation error or invalid parameters).
 */
int hashtable_put(ShmRegion *region, OffsetPtr table_offset, const char *key, const void *value);

/**
 * @brief Retrieve a value by key.
 *
 * @param region        Shared‑memory region.
 * @param table_offset  Offset of the table header.
 * @param key           Null‑terminated key string.
 * @param out          Caller‑provided buffer of at least val_size bytes.
 * @return 0 on success, -1 if key not found.
 */
int hashtable_get(ShmRegion *region, OffsetPtr table_offset, const char *key, void *out);

/**
 * @brief Delete an entry by key.
 *
 * @param region        Shared‑memory region.
 * @param table_offset  Offset of the table header.
 * @param key           Null‑terminated key string.
 * @return 0 if deleted, -1 if key not found.
 */
int hashtable_delete(ShmRegion *region, OffsetPtr table_offset, const char *key);

/**
 * @brief Return the number of entries in the table.
 */
uint64_t hashtable_length(ShmRegion *region, OffsetPtr table_offset);

#endif /* OFFSET_STORE_HASHTABLE_H */