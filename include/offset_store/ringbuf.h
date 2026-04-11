#ifndef OFFSET_STORE_RINGBUF_H
#define OFFSET_STORE_RINGBUF_H

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/**
 * @file ringbuf.h
 * @brief Offset‑based ring buffer (bounded queue) stored in shared memory.
 *
 * The ring buffer provides a fixed‑capacity FIFO queue for producer‑consumer
 * communication. A robust process‑shared mutex protects internal state.
 */

/**
 * @brief Header for a ring buffer.
 *
 * The ring buffer uses a circular array layout. Head points to the next element
 * to dequeue, tail points to the next slot to fill. When head == tail, the
 * buffer is empty. When (tail + 1) % capacity == head, the buffer is full.
 */
typedef struct {
    uint64_t    capacity;       /**< Maximum number of elements (must be > 1). */
    uint64_t    head;            /**< Index of the next element to dequeue. */
    uint64_t    tail;            /**< Index of the next slot to fill. */
    size_t      elem_size;       /**< Size of each element in bytes. */
    OffsetPtr   data_offset;     /**< Offset of the contiguous element buffer. */
    pthread_mutex_t lock;        /**< Robust, process‑shared mutex protecting the buffer. */
} RingBufHeader;

/**
 * @brief Create a new ring buffer.
 *
 * @param region    Shared‑memory region.
 * @param capacity  Maximum number of elements (must be at least 2).
 * @param elem_size Size of each element in bytes (must be non‑zero).
 * @return OffsetPtr to the buffer header, or a null offset on failure.
 */
OffsetPtr ringbuf_create(ShmRegion *region, uint64_t capacity, size_t elem_size);

/**
 * @brief Destroy a ring buffer and free its storage.
 *
 * @param region         Shared‑memory region.
 * @param buf_offset     Offset of the buffer header returned by ringbuf_create.
 */
void ringbuf_destroy(ShmRegion *region, OffsetPtr buf_offset);

/**
 * @brief Push an element to the tail of the queue.
 *
 * @param region    Shared‑memory region.
 * @param buf_offset Offset of the buffer header.
 * @param element   Pointer to the element to copy into the buffer.
 * @return 0 on success, -1 if the buffer is full.
 */
int ringbuf_push(ShmRegion *region, OffsetPtr buf_offset, const void *element);

/**
 * @brief Pop an element from the head of the queue.
 *
 * @param region    Shared‑memory region.
 * @param buf_offset Offset of the buffer header.
 * @param out       Caller‑provided buffer of at least elem_size bytes.
 * @return 0 on success, -1 if the buffer is empty.
 */
int ringbuf_pop(ShmRegion *region, OffsetPtr buf_offset, void *out);

/**
 * @brief Return the current number of elements in the buffer.
 */
uint64_t ringbuf_length(ShmRegion *region, OffsetPtr buf_offset);

/**
 * @brief Return the maximum capacity of the buffer.
 */
uint64_t ringbuf_capacity(ShmRegion *region, OffsetPtr buf_offset);

/**
 * @brief Check if the buffer is empty.
 */
bool ringbuf_is_empty(ShmRegion *region, OffsetPtr buf_offset);

/**
 * @brief Check if the buffer is full.
 */
bool ringbuf_is_full(ShmRegion *region, OffsetPtr buf_offset);

#endif /* OFFSET_STORE_RINGBUF_H */