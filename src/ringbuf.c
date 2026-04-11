/**
 * @file ringbuf.c
 * @brief Implementation of the offset‑based ring buffer (bounded queue) stored in shared memory.
 *
 * The ring buffer provides a fixed‑capacity FIFO queue for producer‑consumer
 * communication. A robust process‑shared mutex protects internal state.
 *
 * Memory layout:
 *   +------------------+
 *   | RingBufHeader    |  (capacity, head, tail, elem_size, data_offset, mutex)
 *   +------------------+
 *   | element buffer   |  (capacity * elem_size bytes, contiguous)
 *   +------------------+
 *
 * Indexing:
 *   - head points to the next element to dequeue
 *   - tail points to the next slot to fill
 *   - Empty when: head == tail
 *   - Full when: (tail + 1) % capacity == head
 *   - One slot is always kept unused to distinguish full from empty
 */

#include "offset_store/ringbuf.h"

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

/**
 * @brief Resolve the buffer header from an {@link OffsetPtr}.
 *
 * Converts the offset that identifies the ring buffer header into a pointer
 * to the RingBufHeader structure stored in shared memory. Validates that the
 * offset is within bounds and covers the full header size.
 *
 * @param[in]  region      The shared‑memory region descriptor.
 * @param[in]  buf_offset  Offset pointing to the buffer header.
 * @param[out] out_hdr     Pointer to where the resolved header pointer will be stored.
 * @return true  if the header was successfully resolved.
 * @return false otherwise (invalid offset, out‑of‑bounds, etc.).
 */
static bool ringbuf_resolve_header(ShmRegion *region, OffsetPtr buf_offset,
                                   RingBufHeader **out_hdr) {
    void *raw = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size, buf_offset,
                               sizeof(RingBufHeader), &raw)) {
        return false;
    }
    *out_hdr = (RingBufHeader *)raw;
    return true;
}

/**
 * @brief Acquire the internal mutex of a ring buffer.
 *
 * The buffer uses a robust, process‑shared {@link pthread_mutex_t}. If the
 * previous owner of the mutex died while holding it, the lock operation returns
 * {@code EOWNERDEAD}. In that case we mark the mutex state as consistent so
 * subsequent operations can continue safely.
 *
 * @param[in] hdr Pointer to the buffer header whose mutex should be locked.
 */
static void ringbuf_lock(RingBufHeader *hdr) {
    int rc = pthread_mutex_lock(&hdr->lock);
    if (rc == EOWNERDEAD) {
#ifdef PTHREAD_MUTEX_ROBUST
        pthread_mutex_consistent(&hdr->lock);
#endif
    }
}

/**
 * @brief Release the internal mutex of a ring buffer.
 *
 * @param[in] hdr Pointer to the buffer header whose mutex should be unlocked.
 */
static void ringbuf_unlock(RingBufHeader *hdr) {
    pthread_mutex_unlock(&hdr->lock);
}

/**
 * @brief Calculate the circular index.
 *
 * @param[in] hdr Pointer to the buffer header.
 * @param[in] i   Raw index (can exceed capacity).
 * @return Index wrapped to [0, capacity).
 */
static inline uint64_t ringbuf_idx(RingBufHeader *hdr, uint64_t i) {
    return i % hdr->capacity;
}

/**
 * @brief Create a new ring buffer.
 *
 * Allocates the buffer header and element data array within the shared region.
 * Initializes a robust process‑shared mutex for thread‑safety. The capacity
 * must be at least 2 (one slot is kept unused to distinguish full from empty).
 *
 * @param region    Shared‑memory region.
 * @param capacity  Maximum number of elements (must be at least 2).
 * @param elem_size Size of each element in bytes (must be non‑zero).
 * @return OffsetPtr to the buffer header, or a null offset on failure.
 */
OffsetPtr ringbuf_create(ShmRegion *region, uint64_t capacity, size_t elem_size) {
    if (capacity < 2 || elem_size == 0) return offset_ptr_null();
    /* Allocate buffer header via object store */
    OffsetPtr hdr_ptr;
    if (object_store_alloc(region, 1, sizeof(RingBufHeader), &hdr_ptr) != OFFSET_STORE_STATUS_OK) {
        return offset_ptr_null();
    }
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, hdr_ptr, &hdr)) {
        return offset_ptr_null();
    }
    /* Initialize header fields */
    hdr->capacity = capacity;
    hdr->head = 0;
    hdr->tail = 0;
    hdr->elem_size = elem_size;
    hdr->data_offset = offset_ptr_null();
    /* Initialize robust, process‑shared mutex */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
#endif
    pthread_mutex_init(&hdr->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    /* Allocate element data buffer */
    size_t data_size = capacity * elem_size;
    void *data_ptr = NULL;
    if (allocator_alloc(region, data_size, 16, &data_ptr) != OFFSET_STORE_STATUS_OK) {
        pthread_mutex_destroy(&hdr->lock);
        object_store_free(region, hdr_ptr);
        return offset_ptr_null();
    }
    /* Zero-initialize the data buffer */
    memset(data_ptr, 0, data_size);
    /* Convert data pointer to offset */
    if (!offset_ptr_try_from_raw(region->base, region->size, data_ptr, &hdr->data_offset)) {
        allocator_free(region, data_ptr);
        pthread_mutex_destroy(&hdr->lock);
        object_store_free(region, hdr_ptr);
        return offset_ptr_null();
    }
    return hdr_ptr;
}

/**
 * @brief Destroy a ring buffer and free its storage.
 *
 * Frees the element data buffer, destroys the mutex, and frees the header.
 *
 * @param region       Shared‑memory region.
 * @param buf_offset   Offset of the buffer header returned by ringbuf_create.
 */
void ringbuf_destroy(ShmRegion *region, OffsetPtr buf_offset) {
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, buf_offset, &hdr)) return;
    ringbuf_lock(hdr);
    /* Free element data buffer */
    if (!offset_ptr_is_null(hdr->data_offset)) {
        void *data = NULL;
        if (offset_ptr_try_resolve(region->base, region->size,
                                  hdr->data_offset, hdr->capacity * hdr->elem_size, &data)) {
            allocator_free(region, data);
        }
    }
    ringbuf_unlock(hdr);
    pthread_mutex_destroy(&hdr->lock);
    object_store_free(region, buf_offset);
}

/**
 * @brief Push an element to the tail of the queue.
 *
 * Fails if the buffer is full (i.e., next(tail) == head).
 *
 * @param region       Shared‑memory region.
 * @param buf_offset   Offset of the buffer header.
 * @param element      Pointer to the element to copy into the buffer.
 * @return 0 on success, -1 if the buffer is full.
 */
int ringbuf_push(ShmRegion *region, OffsetPtr buf_offset, const void *element) {
    if (!element) return -1;
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, buf_offset, &hdr)) return -1;
    ringbuf_lock(hdr);
    /* Check if full: next position after tail equals head */
    uint64_t next_tail = ringbuf_idx(hdr, hdr->tail + 1);
    if (next_tail == hdr->head) {
        ringbuf_unlock(hdr);
        return -1;  /* Buffer full */
    }
    /* Resolve element data buffer */
    void *data = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size,
                              hdr->data_offset, hdr->capacity * hdr->elem_size, &data)) {
        ringbuf_unlock(hdr);
        return -1;
    }
    /* Copy element into buffer at tail position */
    memcpy((unsigned char *)data + hdr->tail * hdr->elem_size, element, hdr->elem_size);
    /* Advance tail */
    hdr->tail = next_tail;
    ringbuf_unlock(hdr);
    return 0;
}

/**
 * @brief Pop an element from the head of the queue.
 *
 * Fails if the buffer is empty (i.e., head == tail).
 *
 * @param region       Shared‑memory region.
 * @param buf_offset   Offset of the buffer header.
 * @param out          Caller‑provided buffer of at least elem_size bytes.
 * @return 0 on success, -1 if the buffer is empty.
 */
int ringbuf_pop(ShmRegion *region, OffsetPtr buf_offset, void *out) {
    if (!out) return -1;
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, buf_offset, &hdr)) return -1;
    ringbuf_lock(hdr);
    /* Check if empty */
    if (hdr->head == hdr->tail) {
        ringbuf_unlock(hdr);
        return -1;  /* Buffer empty */
    }
    /* Resolve element data buffer */
    void *data = NULL;
    if (!offset_ptr_try_resolve(region->base, region->size,
                              hdr->data_offset, hdr->capacity * hdr->elem_size, &data)) {
        ringbuf_unlock(hdr);
        return -1;
    }
    /* Copy element from head position to output */
    memcpy(out, (unsigned char *)data + hdr->head * hdr->elem_size, hdr->elem_size);
    /* Advance head */
    hdr->head = ringbuf_idx(hdr, hdr->head + 1);
    ringbuf_unlock(hdr);
    return 0;
}

/**
 * @brief Return the current number of elements in the buffer.
 *
 * @param region       Shared‑memory region.
 * @param buf_offset   Offset of the buffer header.
 * @return Number of elements currently stored.
 */
uint64_t ringbuf_length(ShmRegion *region, OffsetPtr buf_offset) {
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, buf_offset, &hdr)) return 0;
    ringbuf_lock(hdr);
    if (hdr->tail >= hdr->head) {
        /* Normal case: tail is ahead of head */
        ringbuf_unlock(hdr);
        return hdr->tail - hdr->head;
    }
    /* Wraparound case: tail has wrapped around */
    uint64_t len = hdr->capacity - hdr->head + hdr->tail;
    ringbuf_unlock(hdr);
    return len;
}

/**
 * @brief Return the maximum capacity of the buffer.
 *
 * Note: due to the design (one slot kept unused), the actual number of
 * elements that can be stored is (capacity - 1).
 *
 * @param region       Shared‑memory region.
 * @param buf_offset   Offset of the buffer header.
 * @return Maximum number of elements (including the reserved slot).
 */
uint64_t ringbuf_capacity(ShmRegion *region, OffsetPtr buf_offset) {
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, buf_offset, &hdr)) return 0;
    return hdr->capacity;
}

/**
 * @brief Check if the buffer is empty.
 *
 * @param region       Shared‑memory region.
 * @param buf_offset   Offset of the buffer header.
 * @return true if the buffer contains no elements.
 */
bool ringbuf_is_empty(ShmRegion *region, OffsetPtr buf_offset) {
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, buf_offset, &hdr)) return true;
    ringbuf_lock(hdr);
    bool empty = (hdr->head == hdr->tail);
    ringbuf_unlock(hdr);
    return empty;
}

/**
 * @brief Check if the buffer is full.
 *
 * @param region       Shared‑memory region.
 * @param buf_offset   Offset of the buffer header.
 * @return true if no more elements can be added.
 */
bool ringbuf_is_full(ShmRegion *region, OffsetPtr buf_offset) {
    RingBufHeader *hdr = NULL;
    if (!ringbuf_resolve_header(region, buf_offset, &hdr)) return false;
    ringbuf_lock(hdr);
    bool full = (ringbuf_idx(hdr, hdr->tail + 1) == hdr->head);
    ringbuf_unlock(hdr);
    return full;
}