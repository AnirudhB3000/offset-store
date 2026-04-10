/**
 * @file dynarray.c
 * @brief Implementation of the offset‑based dynamic array (vector) stored in shared memory.
 *
 * The array is allocated as a generic object via the existing object_store facilities.
 * A robust process‑shared mutex inside the header provides internal thread‑safety.
 */

#include "offset_store/dynarray.h"

#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

/* Internal helper: resolve the header from an OffsetPtr */
static bool dynarray_resolve_header(ShmRegion *region, OffsetPtr arr_offset,
                                    DynArrayHeader **out_hdr) {
    void *raw = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, arr_offset,
                               sizeof(DynArrayHeader), &raw)) {
        return false;
    }
    *out_hdr = (DynArrayHeader *)raw;
    return true;
}

/* Internal helper: lock/unlock with robust recovery */
static void dynarray_lock(DynArrayHeader *hdr) {
    int rc = pthread_mutex_lock(&hdr->lock);
    if (rc == EOWNERDEAD) {
        /* Recover consistent state after previous owner died. */
#ifdef PTHREAD_MUTEX_ROBUST
        pthread_mutex_consistent(&hdr->lock);
#endif
    }
}
static void dynarray_unlock(DynArrayHeader *hdr) {
    pthread_mutex_unlock(&hdr->lock);
}

OffsetPtr dynarray_create(ShmRegion *region, size_t elem_size) {
    if (elem_size == 0) return offset_ptr_null();
    OffsetPtr hdr_ptr;
    if (object_store_alloc(region, 1, sizeof(DynArrayHeader), &hdr_ptr) != OFFSET_STORE_STATUS_OK) {
        return offset_ptr_null();
    }
    DynArrayHeader *hdr = NULL;
    if (!dynarray_resolve_header(region, hdr_ptr, &hdr)) {
        return offset_ptr_null();
    }
    // Initialize header fields
    hdr->capacity = 0;
    hdr->length = 0;
    hdr->elem_size = elem_size;
    hdr->data_offset = offset_ptr_null();
    // Initialise robust, process‑shared mutex
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

void dynarray_destroy(ShmRegion *region, OffsetPtr arr_offset) {
    DynArrayHeader *hdr = NULL;
    if (!dynarray_resolve_header(region, arr_offset, &hdr)) return;
    dynarray_lock(hdr);
    if (!offset_ptr_is_null(hdr->data_offset)) {
        // Free payload buffer
        void *payload = NULL;
        if (offset_ptr_try_resolve(region->base, region->size,
                                  hdr->data_offset, hdr->capacity * hdr->elem_size,
                                  &payload)) {
            // Convert to raw pointer for allocator_free via internal helper
            // The payload was allocated with allocator_alloc, so we can free it.
            // We reuse object_store_free which internally calls allocator_free.
            // First get the containing block offset via object_store_free helper.
            // Since payload is not an object_store allocation, we free via allocator.
            // Retrieve the allocation prefix to get block offset.
            // The allocator stores a prefix before the payload; we locate it.
            // Simplify: use allocator_free directly with the payload pointer.
            allocator_free(region, payload);
        }
    }
    dynarray_unlock(hdr);
    // Destroy mutex
    pthread_mutex_destroy(&hdr->lock);
    // Free header via object_store
    object_store_free(region, arr_offset);
}

static int dynarray_grow(ShmRegion *region, DynArrayHeader *hdr, uint64_t new_cap) {
    // Allocate new buffer
    void *new_buf = NULL;
    size_t new_size = new_cap * hdr->elem_size;
    if (allocator_alloc(region, new_size, 16, &new_buf) != OFFSET_STORE_STATUS_OK) {
        return -1;
    }
    // Copy existing data if any
    if (hdr->length > 0 && !offset_ptr_is_null(hdr->data_offset)) {
        void *old_buf = NULL;
        if (offset_ptr_try_resolve(region->base, region->size,
                                   hdr->data_offset,
                                   hdr->length * hdr->elem_size, &old_buf)) {
            memcpy(new_buf, old_buf, hdr->length * hdr->elem_size);
        }
    }
    // Free old buffer
    if (!offset_ptr_is_null(hdr->data_offset)) {
        void *old_buf = NULL;
        if (offset_ptr_try_resolve(region->base, region->size,
                                   hdr->data_offset,
                                   hdr->capacity * hdr->elem_size, &old_buf)) {
            allocator_free(region, old_buf);
        }
    }
    // Store new offset
    OffsetPtr new_offset;
    if (!offset_ptr_try_from_raw(region->base, region->size, new_buf, &new_offset)) {
        // Should not happen
        allocator_free(region, new_buf);
        return -1;
    }
    hdr->data_offset = new_offset;
    hdr->capacity = new_cap;
    return 0;
}

int dynarray_reserve(ShmRegion *region, OffsetPtr arr_offset, uint64_t new_cap) {
    DynArrayHeader *hdr = NULL;
    if (!dynarray_resolve_header(region, arr_offset, &hdr)) return -1;
    dynarray_lock(hdr);
    if (new_cap <= hdr->capacity) {
        dynarray_unlock(hdr);
        return 0;
    }
    int res = dynarray_grow(region, hdr, new_cap);
    dynarray_unlock(hdr);
    return res;
}

int dynarray_push(ShmRegion *region, OffsetPtr arr_offset, const void *elem) {
    if (!elem) return -1;
    DynArrayHeader *hdr = NULL;
    if (!dynarray_resolve_header(region, arr_offset, &hdr)) return -1;
    dynarray_lock(hdr);
    if (hdr->length == hdr->capacity) {
        uint64_t new_cap = hdr->capacity ? hdr->capacity * 2 : 4;
        if (dynarray_grow(region, hdr, new_cap) != 0) {
            dynarray_unlock(hdr);
            return -1;
        }
    }
    // Resolve payload buffer
    void *buf = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size,
                               hdr->data_offset,
                               (hdr->length + 1) * hdr->elem_size, &buf)) {
        dynarray_unlock(hdr);
        return -1;
    }
    // Copy element
    memcpy((unsigned char *)buf + hdr->length * hdr->elem_size, elem, hdr->elem_size);
    hdr->length++;
    dynarray_unlock(hdr);
    return 0;
}

int dynarray_get(ShmRegion *region, OffsetPtr arr_offset, uint64_t index, void *out) {
    if (!out) return -1;
    DynArrayHeader *hdr = NULL;
    if (!dynarray_resolve_header(region, arr_offset, &hdr)) return -1;
    dynarray_lock(hdr);
    if (index >= hdr->length) {
        dynarray_unlock(hdr);
        return -1;
    }
    void *buf = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size,
                               hdr->data_offset,
                               hdr->length * hdr->elem_size, &buf)) {
        dynarray_unlock(hdr);
        return -1;
    }
    memcpy(out, (unsigned char *)buf + index * hdr->elem_size, hdr->elem_size);
    dynarray_unlock(hdr);
    return 0;
}

uint64_t dynarray_length(ShmRegion *region, OffsetPtr arr_offset) {
    DynArrayHeader *hdr = NULL;
    if (!dynarray_resolve_header(region, arr_offset, &hdr)) return 0;
    // Length read under lock for consistency
    dynarray_lock(hdr);
    uint64_t len = hdr->length;
    dynarray_unlock(hdr);
    return len;
}
