/**
 * @file dynlist.c
 * @brief Implementation of an offset‑based intrusive linked list stored in shared memory.
 *
 * The list is allocated as a generic object via the existing object_store API.
 * Each node consists of a {@link DynListNodeHeader} followed by the user payload.
 * A robust process‑shared mutex inside the list header provides internal thread‑safety.
 */

#include "offset_store/dynlist.h"
#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <string.h>
#include <pthread.h>
#include <errno.h>

/**
 * @brief Resolve the list header from an {@link OffsetPtr}.
 */
static bool dynlist_resolve_header(ShmRegion *region, OffsetPtr list_offset,
                                   DynListHeader **out_hdr) {
    void *raw = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, list_offset,
                               sizeof(DynListHeader), &raw)) {
        return false;
    }
    *out_hdr = (DynListHeader *)raw;
    return true;
}

/**
 * @brief Resolve a node header from its offset.
 *
 * This helper translates an {@link OffsetPtr} that points to a node into a
 * pointer to the {@link DynListNodeHeader} stored in the shared region.
 * It validates that the offset is within bounds and that the span covers the
 * full node header.
 *
 * @param[in]  region      Shared‑memory region descriptor.
 * @param[in]  node_offset Offset of the node to resolve.
 * @param[out] out_node   Pointer where the resolved node header pointer will be stored.
 * @return true  if resolution succeeds.
 * @return false otherwise.
 */
static bool dynlist_resolve_node(ShmRegion *region, OffsetPtr node_offset,
                                 DynListNodeHeader **out_node) {
    void *raw = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, node_offset,
                               sizeof(DynListNodeHeader), &raw)) {
        return false;
    }
    *out_node = (DynListNodeHeader *)raw;
    return true;
}

/**
 * @brief Acquire the list mutex with robust recovery.
 */
static void dynlist_lock(DynListHeader *hdr) {
    int rc = pthread_mutex_lock(&hdr->lock);
    if (rc == EOWNERDEAD) {
#ifdef PTHREAD_MUTEX_ROBUST
        pthread_mutex_consistent(&hdr->lock);
#endif
    }
}

/**
 * @brief Release the list mutex.
 */
static void dynlist_unlock(DynListHeader *hdr) {
    pthread_mutex_unlock(&hdr->lock);
}

/**
 * @brief Create a new linked list.
 *
 * Allocates a {@link DynListHeader} via the generic object store, initializes
 * its fields (length, element size, head/tail offsets) and creates a robust
 * process‑shared mutex for internal synchronization.
 *
 * @param[in] region    Shared‑memory region.
 * @param[in] elem_size Size of each list element (must be non‑zero).
 * @return OffsetPtr to the list header, or a null offset on failure.
 */
OffsetPtr dynlist_create(ShmRegion *region, size_t elem_size) {
    if (elem_size == 0) return offset_ptr_null();
    OffsetPtr hdr_ptr;
    if (object_store_alloc(region, 1, sizeof(DynListHeader), &hdr_ptr) != OFFSET_STORE_STATUS_OK) {
        return offset_ptr_null();
    }
    DynListHeader *hdr = NULL;
    if (!dynlist_resolve_header(region, hdr_ptr, &hdr)) return offset_ptr_null();
    hdr->length = 0;
    hdr->elem_size = elem_size;
    hdr->head = offset_ptr_null();
    hdr->tail = offset_ptr_null();
    // initialise robust mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
    pthread_mutex_init(&hdr->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    return hdr_ptr;
}

/**
 * @brief Destroy a linked list and free all its nodes.
 *
 * Traverses the list, frees each node's allocation, destroys the internal
 * mutex, and finally frees the list header via {@link object_store_free}.
 *
 * @param[in] region       Shared‑memory region.
 * @param[in] list_offset  Offset of the list header returned by {@link dynlist_create}.
 */
void dynlist_destroy(ShmRegion *region, OffsetPtr list_offset) {
    DynListHeader *hdr = NULL;
    if (!dynlist_resolve_header(region, list_offset, &hdr)) return;
    dynlist_lock(hdr);
    // Walk and free all nodes
    OffsetPtr cur = hdr->head;
    while (!offset_ptr_is_null(cur)) {
        DynListNodeHeader *node = NULL;
        if (!dynlist_resolve_node(region, cur, &node)) break;
        // Compute payload pointer (immediately after header)
        void *payload = (unsigned char *)node + sizeof(DynListNodeHeader);
        // Free node buffer (the whole allocation)
        allocator_free(region, payload);
        cur = node->next;
    }
    dynlist_unlock(hdr);
    pthread_mutex_destroy(&hdr->lock);
    object_store_free(region, list_offset);
}

/**
 * @brief Insert a new node into the list.
 *
 * This internal helper allocates a node (header + payload), copies the
 * element data, and links the node either after the given @p after offset or
 * as the sole node when the list is empty. It updates the list's head/tail
 * pointers and increments the length counter.
 *
 * @param[in] region   Shared‑memory region.
 * @param[in] hdr      Pointer to the list header (already locked by the caller).
 * @param[in] after    Offset of the node after which the new node should be inserted.
 *                     Use {@link offset_ptr_null} to insert as the first node.
 * @param[in] elem     Pointer to the element data to store in the new node.
 * @return 0 on success, -1 on allocation failure.
 */
static int dynlist_insert_node(ShmRegion *region, DynListHeader *hdr, OffsetPtr after, const void *elem) {
    // allocate node (header + payload)
    size_t node_total = sizeof(DynListNodeHeader) + hdr->elem_size;
    void *buf = NULL;
    if (allocator_alloc(region, node_total, 16, &buf) != OFFSET_STORE_STATUS_OK) return -1;
    // Resolve node offset
    OffsetPtr node_offset;
    if (!offset_ptr_try_from_raw(region->base, region->size, buf, &node_offset)) {
        allocator_free(region, buf);
        return -1;
    }
    // Initialize node header
    DynListNodeHeader *node = (DynListNodeHeader *)buf;
    node->next = offset_ptr_null();
    node->prev = offset_ptr_null();
    // Copy payload
    memcpy((unsigned char *)buf + sizeof(DynListNodeHeader), elem, hdr->elem_size);
    // Insert into list
    if (offset_ptr_is_null(after)) {
        // Insert at head (used for push_front when list empty)
        hdr->head = node_offset;
        hdr->tail = node_offset;
    } else {
        // Insert after existing node
        DynListNodeHeader *after_node = NULL;
        if (!dynlist_resolve_node(region, after, &after_node)) {
            allocator_free(region, buf);
            return -1;
        }
        // Link new node
        node->prev = after;
        node->next = after_node->next;
        after_node->next = node_offset;
        if (!offset_ptr_is_null(node->next)) {
            DynListNodeHeader *next_node = NULL;
            if (dynlist_resolve_node(region, node->next, &next_node)) {
                next_node->prev = node_offset;
            }
        } else {
            // was tail
            hdr->tail = node_offset;
        }
    }
    hdr->length++;
    return 0;
}

/**
 * @brief Append an element to the end of the list.
 *
 * The list is locked, a new node is allocated and linked after the current
 * tail, and the tail pointer as well as the length counter are updated.
 *
 * @param[in] region      Shared‑memory region.
 * @param[in] list_offset Offset of the list header.
 * @param[in] elem        Pointer to the element data to copy into the list.
 * @return 0 on success, non‑zero on allocation failure.
 */
int dynlist_push_back(ShmRegion *region, OffsetPtr list_offset, const void *elem) {
    DynListHeader *hdr = NULL;
    if (!dynlist_resolve_header(region, list_offset, &hdr)) return -1;
    dynlist_lock(hdr);
    int rc = dynlist_insert_node(region, hdr, hdr->tail, elem);
    dynlist_unlock(hdr);
    return rc;
}

/**
 * @brief Insert an element at the beginning of the list.
 *
 * The list is locked, a new node is allocated and linked as the new head,
 * and the head/tail pointers and length are updated accordingly.
 *
 * @param[in] region      Shared‑memory region.
 * @param[in] list_offset Offset of the list header.
 * @param[in] elem        Pointer to the element data.
 * @return 0 on success, non‑zero on allocation failure.
 */
int dynlist_push_front(ShmRegion *region, OffsetPtr list_offset, const void *elem) {
    DynListHeader *hdr = NULL;
    if (!dynlist_resolve_header(region, list_offset, &hdr)) return -1;
    dynlist_lock(hdr);
    // Insert after null (will become new head)
    // If list is empty, treat as tail as well.
    if (offset_ptr_is_null(hdr->head)) {
        // empty list: insert node and set both head and tail
        int rc = dynlist_insert_node(region, hdr, offset_ptr_null(), elem);
        dynlist_unlock(hdr);
        return rc;
    }
    // Insert before current head: we will insert after null and then swap head.
    // Simpler: allocate node and set it as new head manually.
    size_t node_total = sizeof(DynListNodeHeader) + hdr->elem_size;
    void *buf = NULL;
    if (allocator_alloc(region, node_total, 16, &buf) != OFFSET_STORE_STATUS_OK) {
        dynlist_unlock(hdr);
        return -1;
    }
    OffsetPtr node_offset;
    if (!offset_ptr_try_from_raw(region->base, region->size, buf, &node_offset)) {
        allocator_free(region, buf);
        dynlist_unlock(hdr);
        return -1;
    }
    DynListNodeHeader *node = (DynListNodeHeader *)buf;
    node->prev = offset_ptr_null();
    node->next = hdr->head;
    memcpy((unsigned char *)buf + sizeof(DynListNodeHeader), elem, hdr->elem_size);
    // Update previous head's prev pointer
    DynListNodeHeader *old_head = NULL;
    if (dynlist_resolve_node(region, hdr->head, &old_head)) {
        old_head->prev = node_offset;
    }
    hdr->head = node_offset;
    if (offset_ptr_is_null(hdr->tail)) {
        hdr->tail = node_offset;
    }
    hdr->length++;
    dynlist_unlock(hdr);
    return 0;
}

int dynlist_get(ShmRegion *region, OffsetPtr list_offset, uint64_t index, void *out) {
    if (!out) return -1;
    DynListHeader *hdr = NULL;
    if (!dynlist_resolve_header(region, list_offset, &hdr)) return -1;
    dynlist_lock(hdr);
    if (index >= hdr->length) { dynlist_unlock(hdr); return -1; }
    OffsetPtr cur = hdr->head;
    for (uint64_t i = 0; i < index; ++i) {
        DynListNodeHeader *node = NULL;
        if (!dynlist_resolve_node(region, cur, &node)) { dynlist_unlock(hdr); return -1; }
        cur = node->next;
    }
    DynListNodeHeader *target = NULL;
    if (!dynlist_resolve_node(region, cur, &target)) { dynlist_unlock(hdr); return -1; }
    void *payload = (unsigned char *)target + sizeof(DynListNodeHeader);
    memcpy(out, payload, hdr->elem_size);
    dynlist_unlock(hdr);
    return 0;
}

uint64_t dynlist_length(ShmRegion *region, OffsetPtr list_offset) {
    DynListHeader *hdr = NULL;
    if (!dynlist_resolve_header(region, list_offset, &hdr)) return 0;
    dynlist_lock(hdr);
    uint64_t len = hdr->length;
    dynlist_unlock(hdr);
    return len;
}
