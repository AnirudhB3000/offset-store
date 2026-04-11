/**
 * @file hashtable.c
 * @brief Implementation of the offset‑based hash table stored in shared memory.
 *
 * The hash table uses chaining for collision resolution. Each bucket contains
 * a pointer to a linked list of entries. All bucket metadata is stored in the
 * shared region. A robust process‑shared mutex protects internal state.
 *
 * Memory layout:
 *   +------------------+
 *   | HashTableHeader |  (num_buckets, key_size, val_size, buckets offset, mutex)
 *   +------------------+
 *   | bucket array     |  (num_buckets * sizeof(OffsetPtr))
 *   +------------------+
 *   | entries          |  (one allocation per key-value pair)
 *   +------------------+
 *   | key strings      |  (length + data + null, per entry)
 *   +------------------+
 *   | value buffers    |  (val_size bytes, per entry)
 *   +------------------+
 */

#include "offset_store/hashtable.h"

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Resolve the table header from an {@link OffsetPtr}.
 *
 * Converts the offset that identifies the hash table header into a pointer
 * to the HashTableHeader structure stored in shared memory. Validates that
 * the offset is within bounds and covers the full header size.
 *
 * @param[in]  region       The shared‑memory region descriptor.
 * @param[in]  tbl_offset   Offset pointing to the table header.
 * @param[out] out_hdr      Pointer to where the resolved header pointer will be stored.
 * @return true  if the header was successfully resolved.
 * @return false otherwise (invalid offset, out‑of‑bounds, etc.).
 */
static bool hashtable_resolve_header(ShmRegion *region, OffsetPtr tbl_offset,
                                     HashTableHeader **out_hdr) {
    void *raw = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, tbl_offset,
                               sizeof(HashTableHeader), &raw)) {
        return false;
    }
    *out_hdr = (HashTableHeader *)raw;
    return true;
}

/**
 * @brief Acquire the internal mutex of a hash table.
 *
 * The table uses a robust, process‑shared {@link pthread_mutex_t}. If the
 * previous owner of the mutex died while holding it, the lock operation returns
 * {@code EOWNERDEAD}. In that case we mark the mutex state as consistent so
 * subsequent operations can continue safely.
 *
 * @param[in] hdr Pointer to the table header whose mutex should be locked.
 */
static void hashtable_lock(HashTableHeader *hdr) {
    int rc = pthread_mutex_lock(&hdr->lock);
    if (rc == EOWNERDEAD) {
#ifdef PTHREAD_MUTEX_ROBUST
        pthread_mutex_consistent(&hdr->lock);
#endif
    }
}

/**
 * @brief Release the internal mutex of a hash table.
 *
 * @param[in] hdr Pointer to the table header whose mutex should be unlocked.
 */
static void hashtable_unlock(HashTableHeader *hdr) {
    pthread_mutex_unlock(&hdr->lock);
}

/**
 * @brief Simple hash function for null‑terminated strings.
 *
 * Uses the djb2 hash algorithm (variant): h = h * 33 + c. This provides
 * reasonable distribution for typical string keys.
 *
 * @param key         Null‑terminated string key.
 * @param num_buckets Number of buckets in the table (for modulo).
 * @return Hash value in the range [0, num_buckets).
 */
static uint64_t hashtable_hash(const char *key, uint64_t num_buckets) {
    uint64_t h = 5381;
    int c;
    while ((c = *key++)) {
        h = ((h << 5) + h) + c;  /* h * 33 + c */
    }
    return h % num_buckets;
}

/**
 * @brief Allocate a variable‑length string in shared memory.
 *
 * Strings are stored as: length (uint32_t) + character data + null terminator.
 * The length field allows efficient string comparison without scanning twice.
 *
 * @param region   Shared‑memory region.
 * @param str      Null‑terminated source string.
 * @param max_len  Maximum length to store (truncates if exceeded).
 * @return OffsetPtr to the stored string, or null offset on failure.
 */
static OffsetPtr hashtable_alloc_string(ShmRegion *region, const char *str, size_t max_len) {
    size_t len = strlen(str);
    if (len > max_len) len = max_len;
    size_t total = sizeof(uint32_t) + len + 1;
    void *ptr = NULL;
    if (allocator_alloc(region, total, 16, &ptr) != OFFSET_STORE_STATUS_OK) {
        return offset_ptr_null();
    }
    /* Store length first, then data */
    uint32_t *len_store = (uint32_t *)ptr;
    *len_store = (uint32_t)len;
    char *data = (char *)(len_store + 1);
    memcpy(data, str, len);
    data[len] = '\0';
    /* Convert allocated pointer to offset */
    OffsetPtr off;
    if (!offset_ptr_try_from_raw(region->base, region->size, ptr, &off)) {
        allocator_free(region, ptr);
        return offset_ptr_null();
    }
    return off;
}

/**
 * @brief Resolve a stored string from its offset.
 *
 * @param region    Shared‑memory region.
 * @param str_off   Offset of the stored string.
 * @return Pointer to the null‑terminated string content, or NULL on failure.
 *         The caller must not free this pointer.
 */
static char *hashtable_resolve_string(ShmRegion *region, OffsetPtr str_off) {
    void *raw = NULL;
    /* First resolve just the length field */
    if (!offset_ptr_try_resolve(region->base, region->size, str_off, sizeof(uint32_t), &raw)) {
        return NULL;
    }
    uint32_t len = *(uint32_t *)raw;
    /* Now resolve full string: length + data + null */
    if (!offset_ptr_try_resolve(region->base, region->size, str_off,
                                sizeof(uint32_t) + len + 1, &raw)) {
        return NULL;
    }
    /* Skip length field and return pointer to string data */
    return (char *)raw + sizeof(uint32_t);
}

/**
 * @brief Compare two key strings by their stored offsets.
 *
 * @param region  Shared‑memory region.
 * @param key1    Offset of the first key string.
 * @param key2    Second key as null‑terminated string.
 * @return true if keys match exactly.
 */
static bool hashtable_keys_equal(ShmRegion *region, OffsetPtr key1_off, const char *key2) {
    char *key1 = hashtable_resolve_string(region, key1_off);
    if (!key1) return false;
    /* Get length from memory before the string */
    uint32_t len1 = *(uint32_t *)((char *)key1 - sizeof(uint32_t));
    size_t len2 = strlen(key2);
    if (len1 != len2) return false;
    return strncmp(key1, key2, len1) == 0;
}

/**
 * @brief Free a string allocation.
 *
 * @param region  Shared‑memory region.
 * @param str_off Offset of the string to free.
 */
static void hashtable_free_string(ShmRegion *region, OffsetPtr str_off) {
    void *raw = NULL;
    /* Resolve just enough to get the start pointer for allocator_free */
    if (offset_ptr_try_resolve(region->base, region->size, str_off, sizeof(uint32_t), &raw)) {
        allocator_free(region, raw);
    }
}

/**
 * @brief Allocate an entry node in shared memory.
 *
 * Each entry stores offsets to its key string, value buffer, and the next
 * entry in the bucket chain.
 *
 * @param region    Shared‑memory region.
 * @param key_off   Offset of the key string.
 * @param val_off   Offset of the value buffer.
 * @param next_off  Offset of the next entry (or null for end of chain).
 * @return OffsetPtr to the entry, or null offset on failure.
 */
static OffsetPtr hashtable_alloc_entry(ShmRegion *region, OffsetPtr key_off,
                                      OffsetPtr val_off, OffsetPtr next_off) {
    void *ptr = NULL;
    if (allocator_alloc(region, sizeof(HashTableEntry), 16, &ptr) != OFFSET_STORE_STATUS_OK) {
        return offset_ptr_null();
    }
    HashTableEntry *entry = (HashTableEntry *)ptr;
    entry->key_offset = key_off;
    entry->value_offset = val_off;
    entry->next = next_off;
    OffsetPtr off;
    if (!offset_ptr_try_from_raw(region->base, region->size, ptr, &off)) {
        allocator_free(region, ptr);
        return offset_ptr_null();
    }
    return off;
}

/**
 * @brief Create a new hash table.
 *
 * Allocates the table header and bucket array within the shared region.
 * Initializes a robust process‑shared mutex for thread‑safety.
 *
 * @param region      Shared‑memory region.
 * @param num_buckets Number of hash buckets (must be non‑zero).
 * @param key_size    Maximum key string length (excluding null terminator).
 * @param val_size    Size of each value in bytes.
 * @return OffsetPtr to the table header, or a null offset on failure.
 */
OffsetPtr hashtable_create(ShmRegion *region, uint64_t num_buckets, size_t key_size, size_t val_size) {
    if (num_buckets == 0 || key_size == 0 || val_size == 0) {
        return offset_ptr_null();
    }
    /* Allocate table header via object store */
    OffsetPtr hdr_ptr;
    if (object_store_alloc(region, 1, sizeof(HashTableHeader), &hdr_ptr) != OFFSET_STORE_STATUS_OK) {
        return offset_ptr_null();
    }
    HashTableHeader *hdr = NULL;
    if (!hashtable_resolve_header(region, hdr_ptr, &hdr)) {
        return offset_ptr_null();
    }
    hdr->num_buckets = num_buckets;
    hdr->key_size = key_size;
    hdr->val_size = val_size;
    hdr->buckets = offset_ptr_null();
    /* Initialize robust, process‑shared mutex */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
    pthread_mutex_init(&hdr->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    /* Allocate bucket array (array of OffsetPtr for chain heads) */
    size_t bucket_arr_size = num_buckets * sizeof(OffsetPtr);
    void *bucket_ptr = NULL;
    if (allocator_alloc(region, bucket_arr_size, 16, &bucket_ptr) != OFFSET_STORE_STATUS_OK) {
        pthread_mutex_destroy(&hdr->lock);
        object_store_free(region, hdr_ptr);
        return offset_ptr_null();
    }
    /* Initialize all bucket pointers to null */
    memset(bucket_ptr, 0, bucket_arr_size);
    if (!offset_ptr_try_from_raw(region->base, region->size, bucket_ptr, &hdr->buckets)) {
        allocator_free(region, bucket_ptr);
        pthread_mutex_destroy(&hdr->lock);
        object_store_free(region, hdr_ptr);
        return offset_ptr_null();
    }
    return hdr_ptr;
}

/**
 * @brief Destroy a hash table and free all entries.
 *
 * Traverses each bucket chain and frees: key strings, value buffers,
 * and entry nodes. Also frees the bucket array and destroys the mutex.
 *
 * @param region       Shared‑memory region.
 * @param table_offset Offset of the table header returned by hashtable_create.
 */
void hashtable_destroy(ShmRegion *region, OffsetPtr table_offset) {
    HashTableHeader *hdr = NULL;
    if (!hashtable_resolve_header(region, table_offset, &hdr)) return;
    hashtable_lock(hdr);
    /* Resolve bucket array */
    OffsetPtr *buckets = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, hdr->buckets,
                               hdr->num_buckets * sizeof(OffsetPtr), (void **)&buckets)) {
        hashtable_unlock(hdr);
        return;
    }
    /* Free each bucket chain */
    for (uint64_t i = 0; i < hdr->num_buckets; i++) {
        OffsetPtr entry_off = buckets[i];
        while (!offset_ptr_is_null(entry_off)) {
            void *entry_raw = NULL;
            if (!offset_ptr_try_resolve(region->base, region->size, entry_off,
                                       sizeof(HashTableEntry), &entry_raw)) {
                break;
            }
            HashTableEntry *entry = (HashTableEntry *)entry_raw;
            OffsetPtr next = entry->next;
            /* Free key string */
            if (!offset_ptr_is_null(entry->key_offset)) {
                hashtable_free_string(region, entry->key_offset);
            }
            /* Free value buffer */
            if (!offset_ptr_is_null(entry->value_offset)) {
                void *val = NULL;
                if (offset_ptr_try_resolve(region->base, region->size,
                                         entry->value_offset, hdr->val_size, &val)) {
                    allocator_free(region, val);
                }
            }
            /* Free entry node */
            allocator_free(region, entry);
            entry_off = next;
        }
    }
    /* Free bucket array */
    allocator_free(region, buckets);
    hashtable_unlock(hdr);
    pthread_mutex_destroy(&hdr->lock);
    object_store_free(region, table_offset);
}

/**
 * @brief Insert or update a key‑value pair.
 *
 * If the key already exists, updates the value. Otherwise inserts a new entry
 * at the head of the appropriate bucket chain. Uses doubling growth strategy
 * implicitly via bucket insertion.
 *
 * @param region       Shared‑memory region.
 * @param table_offset Offset of the table header.
 * @param key          Null‑terminated key string.
 * @param value        Pointer to the value data to store.
 * @return 0 on success, non‑zero on failure (allocation error or invalid parameters).
 */
int hashtable_put(ShmRegion *region, OffsetPtr table_offset, const char *key, const void *value) {
    if (!key || !value) return -1;
    HashTableHeader *hdr = NULL;
    if (!hashtable_resolve_header(region, table_offset, &hdr)) return -1;
    hashtable_lock(hdr);
    /* Hash and find bucket */
    uint64_t idx = hashtable_hash(key, hdr->num_buckets);
    OffsetPtr *buckets = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, hdr->buckets,
                               hdr->num_buckets * sizeof(OffsetPtr), (void **)&buckets)) {
        hashtable_unlock(hdr);
        return -1;
    }
    /* Check for existing key and update */
    OffsetPtr entry_off = buckets[idx];
    while (!offset_ptr_is_null(entry_off)) {
        void *entry_raw = NULL;
        if (!offset_ptr_try_resolve(region->base, region->size, entry_off,
                                   sizeof(HashTableEntry), &entry_raw)) {
            break;
        }
        HashTableEntry *entry = (HashTableEntry *)entry_raw;
        if (hashtable_keys_equal(region, entry->key_offset, key)) {
            /* Update existing value: free old, allocate new */
            if (!offset_ptr_is_null(entry->value_offset)) {
                void *old_val = NULL;
                if (offset_ptr_try_resolve(region->base, region->size,
                                         entry->value_offset, hdr->val_size, &old_val)) {
                    allocator_free(region, old_val);
                }
            }
            /* Allocate new value buffer */
            void *new_val = NULL;
            if (allocator_alloc(region, hdr->val_size, 16, &new_val) != OFFSET_STORE_STATUS_OK) {
                hashtable_unlock(hdr);
                return -1;
            }
            memcpy(new_val, value, hdr->val_size);
            OffsetPtr new_val_off;
            if (!offset_ptr_try_from_raw(region->base, region->size, new_val, &new_val_off)) {
                allocator_free(region, new_val);
                hashtable_unlock(hdr);
                return -1;
            }
            entry->value_offset = new_val_off;
            hashtable_unlock(hdr);
            return 0;
        }
        entry_off = entry->next;
    }
    /* Insert new entry at bucket head */
    OffsetPtr key_off = hashtable_alloc_string(region, key, hdr->key_size);
    if (offset_ptr_is_null(key_off)) {
        hashtable_unlock(hdr);
        return -1;
    }
    /* Allocate value buffer */
    void *val_ptr = NULL;
    if (allocator_alloc(region, hdr->val_size, 16, &val_ptr) != OFFSET_STORE_STATUS_OK) {
        hashtable_free_string(region, key_off);
        hashtable_unlock(hdr);
        return -1;
    }
    memcpy(val_ptr, value, hdr->val_size);
    OffsetPtr val_off;
    if (!offset_ptr_try_from_raw(region->base, region->size, val_ptr, &val_off)) {
        allocator_free(region, val_ptr);
        hashtable_free_string(region, key_off);
        hashtable_unlock(hdr);
        return -1;
    }
    /* Allocate entry node */
    OffsetPtr new_entry_off = hashtable_alloc_entry(region, key_off, val_off, buckets[idx]);
    if (offset_ptr_is_null(new_entry_off)) {
        allocator_free(region, val_ptr);
        hashtable_free_string(region, key_off);
        hashtable_unlock(hdr);
        return -1;
    }
    /* Insert at head of chain */
    buckets[idx] = new_entry_off;
    hashtable_unlock(hdr);
    return 0;
}

/**
 * @brief Retrieve a value by key.
 *
 * @param region       Shared‑memory region.
 * @param table_offset Offset of the table header.
 * @param key          Null‑terminated key string.
 * @param out          Caller‑provided buffer of at least val_size bytes.
 * @return 0 on success, -1 if key not found.
 */
int hashtable_get(ShmRegion *region, OffsetPtr table_offset, const char *key, void *out) {
    if (!key || !out) return -1;
    HashTableHeader *hdr = NULL;
    if (!hashtable_resolve_header(region, table_offset, &hdr)) return -1;
    hashtable_lock(hdr);
    /* Hash and find bucket */
    uint64_t idx = hashtable_hash(key, hdr->num_buckets);
    OffsetPtr *buckets = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, hdr->buckets,
                               hdr->num_buckets * sizeof(OffsetPtr), (void **)&buckets)) {
        hashtable_unlock(hdr);
        return -1;
    }
    /* Search bucket chain */
    OffsetPtr entry_off = buckets[idx];
    while (!offset_ptr_is_null(entry_off)) {
        void *entry_raw = NULL;
        if (!offset_ptr_try_resolve(region->base, region->size, entry_off,
                                   sizeof(HashTableEntry), &entry_raw)) {
            break;
        }
        HashTableEntry *entry = (HashTableEntry *)entry_raw;
        if (hashtable_keys_equal(region, entry->key_offset, key)) {
            /* Found: copy value to output */
            void *val = NULL;
            if (!offset_ptr_try_resolve(region->base, region->size,
                                      entry->value_offset, hdr->val_size, &val)) {
                hashtable_unlock(hdr);
                return -1;
            }
            memcpy(out, val, hdr->val_size);
            hashtable_unlock(hdr);
            return 0;
        }
        entry_off = entry->next;
    }
    hashtable_unlock(hdr);
    return -1;
}

/**
 * @brief Delete an entry by key.
 *
 * Removes the entry from its bucket chain and frees all associated
 * resources: key string, value buffer, and entry node.
 *
 * @param region       Shared‑memory region.
 * @param table_offset Offset of the table header.
 * @param key          Null‑terminated key string.
 * @return 0 if deleted, -1 if key not found.
 */
int hashtable_delete(ShmRegion *region, OffsetPtr table_offset, const char *key) {
    if (!key) return -1;
    HashTableHeader *hdr = NULL;
    if (!hashtable_resolve_header(region, table_offset, &hdr)) return -1;
    hashtable_lock(hdr);
    /* Hash and find bucket */
    uint64_t idx = hashtable_hash(key, hdr->num_buckets);
    OffsetPtr *buckets = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, hdr->buckets,
                               hdr->num_buckets * sizeof(OffsetPtr), (void **)&buckets)) {
        hashtable_unlock(hdr);
        return -1;
    }
    /* Search chain, keeping track of previous node */
    OffsetPtr prev_off = offset_ptr_null();
    OffsetPtr entry_off = buckets[idx];
    while (!offset_ptr_is_null(entry_off)) {
        void *entry_raw = NULL;
        if (!offset_ptr_try_resolve(region->base, region->size, entry_off,
                                   sizeof(HashTableEntry), &entry_raw)) {
            break;
        }
        HashTableEntry *entry = (HashTableEntry *)entry_raw;
        if (hashtable_keys_equal(region, entry->key_offset, key)) {
            /* Unlink from chain */
            if (offset_ptr_is_null(prev_off)) {
                /* First in chain */
                buckets[idx] = entry->next;
            } else {
                /* Not first: update previous node's next pointer */
                void *prev_raw = NULL;
                if (offset_ptr_try_resolve(region->base, region->size, prev_off,
                                         sizeof(HashTableEntry), &prev_raw)) {
                    HashTableEntry *prev_entry = (HashTableEntry *)prev_raw;
                    prev_entry->next = entry->next;
                }
            }
            /* Free resources */
            if (!offset_ptr_is_null(entry->key_offset)) {
                hashtable_free_string(region, entry->key_offset);
            }
            if (!offset_ptr_is_null(entry->value_offset)) {
                void *val = NULL;
                if (offset_ptr_try_resolve(region->base, region->size,
                                         entry->value_offset, hdr->val_size, &val)) {
                    allocator_free(region, val);
                }
            }
            allocator_free(region, entry);
            hashtable_unlock(hdr);
            return 0;
        }
        prev_off = entry_off;
        entry_off = entry->next;
    }
    hashtable_unlock(hdr);
    return -1;
}

/**
 * @brief Return the number of entries in the table.
 *
 * Traverses all buckets and counts entries. O(num_buckets + num_entries).
 *
 * @param region       Shared‑memory region.
 * @param table_offset Offset of the table header.
 * @return Number of stored key‑value pairs.
 */
uint64_t hashtable_length(ShmRegion *region, OffsetPtr table_offset) {
    HashTableHeader *hdr = NULL;
    if (!hashtable_resolve_header(region, table_offset, &hdr)) return 0;
    hashtable_lock(hdr);
    OffsetPtr *buckets = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, hdr->buckets,
                               hdr->num_buckets * sizeof(OffsetPtr), (void **)&buckets)) {
        hashtable_unlock(hdr);
        return 0;
    }
    uint64_t count = 0;
    for (uint64_t i = 0; i < hdr->num_buckets; i++) {
        OffsetPtr entry_off = buckets[i];
        while (!offset_ptr_is_null(entry_off)) {
            void *entry_raw = NULL;
            if (!offset_ptr_try_resolve(region->base, region->size, entry_off,
                                       sizeof(HashTableEntry), &entry_raw)) {
                break;
            }
            HashTableEntry *entry = (HashTableEntry *)entry_raw;
            count++;
            entry_off = entry->next;
        }
    }
    hashtable_unlock(hdr);
    return count;
}