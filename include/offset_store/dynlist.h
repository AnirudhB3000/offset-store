#ifndef OFFSET_STORE_DYNLIST_H
#define OFFSET_STORE_DYNLIST_H

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/**
 * @file dynlist.h
 * @brief Offset‑based linked (intrusive) list stored in shared memory.
 *
 * The list stores user data directly after each node header, allowing any
 * element type to be stored without additional heap allocations. A robust
 * process‑shared mutex in the list header provides internal thread‑safety.
 */

/**
 * @brief Header for a list node.
 *
 * The node header contains offset pointers to the next and previous nodes so
 * that the list can be traversed from either direction. The actual payload
 * follows this header in memory and occupies @p elem_size bytes.
 */
typedef struct {
    OffsetPtr next;   /**< Offset of the next node (or null). */
    OffsetPtr prev;   /**< Offset of the previous node (or null). */
} DynListNodeHeader;

/**
 * @brief Header for the linked list itself.
 *
 * The list header lives in shared memory and tracks the head/tail offsets,
 * element size, current length, and a robust mutex for synchronization.
 */
typedef struct {
    uint64_t    length;      /**< Number of elements in the list. */
    size_t      elem_size;   /**< Size of each element in bytes. */
    OffsetPtr   head;        /**< Offset of the first node. */
    OffsetPtr   tail;        /**< Offset of the last node. */
    pthread_mutex_t lock;    /**< Robust, process‑shared mutex protecting the list. */
} DynListHeader;

/**
 * @brief Create a new linked list.
 *
 * @param region    Shared‑memory region.
 * @param elem_size Size of each element (must be non‑zero).
 * @return OffsetPtr to the list header, or a null offset on failure.
 */
OffsetPtr dynlist_create(ShmRegion *region, size_t elem_size);

/**
 * @brief Destroy a linked list and free all its nodes.
 *
 * @param region Shared‑memory region.
 * @param list_offset Offset of the list header returned by dynlist_create.
 */
void dynlist_destroy(ShmRegion *region, OffsetPtr list_offset);

/**
 * @brief Append an element to the end of the list.
 *
 * @param region Shared‑memory region.
 * @param list_offset Offset of the list header.
 * @param element Pointer to the element data to copy into the list.
 * @return 0 on success, non‑zero on allocation failure.
 */
int dynlist_push_back(ShmRegion *region, OffsetPtr list_offset, const void *element);

/**
 * @brief Insert an element at the beginning of the list.
 *
 * @param region Shared‑memory region.
 * @param list_offset Offset of the list header.
 * @param element Pointer to the element data.
 * @return 0 on success, non‑zero on allocation failure.
 */
int dynlist_push_front(ShmRegion *region, OffsetPtr list_offset, const void *element);

/**
 * @brief Retrieve the element at the given index.
 *
 * @param region Shared‑memory region.
 * @param list_offset Offset of the list header.
 * @param index Zero‑based index (must be less than length).
 * @param out   Caller‑provided buffer of at least elem_size bytes.
 * @return 0 on success, non‑zero on out‑of‑range.
 */
int dynlist_get(ShmRegion *region, OffsetPtr list_offset, uint64_t index, void *out);

/**
 * @brief Return the current number of elements in the list.
 */
uint64_t dynlist_length(ShmRegion *region, OffsetPtr list_offset);

#endif /* OFFSET_STORE_DYNLIST_H */
