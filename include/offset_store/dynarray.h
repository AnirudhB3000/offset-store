#ifndef OFFSET_STORE_DYNARRAY_H
#define OFFSET_STORE_DYNARRAY_H

#include "offset_store/allocator.h"
#include "offset_store/object_store.h"
#include "offset_store/offset_ptr.h"
#include "offset_store/shm_region.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/**
 * @file dynarray.h
 * @brief Offset‑based dynamic array (vector) stored in shared memory.
 *
 * The array is allocated as a generic object via the existing object_store
 * facilities. A robust process‑shared mutex inside the header provides internal
 * thread‑safety.
 */

/**
 * @brief Header stored in the shared region for each dynamic array.
 *
 * All fields are plain scalars or offset pointers that can be safely persisted
 * in shared memory.
 */
typedef struct {
    uint64_t    capacity;       /**< Number of elements the payload buffer can hold */
    uint64_t    length;         /**< Current number of valid elements */
    size_t      elem_size;      /**< Size of a single element in bytes */
    OffsetPtr   data_offset;    /**< Offset of the contiguous payload buffer */
    pthread_mutex_t lock;        /**< Robust, process‑shared mutex protecting the array */
} DynArrayHeader;

/**
 * @brief Creates a new dynamic array object.
 *
 * @param region   Shared‑memory region.
 * @param elem_size Size of each element in bytes (must be > 0).
 * @return OffsetPtr to the array header on success; a null offset on failure.
 */
OffsetPtr dynarray_create(ShmRegion *region, size_t elem_size);

/**
 * @brief Destroys a dynamic array and frees its payload.
 *
 * @param region Shared‑memory region.
 * @param array_offset Offset of the array header returned by dynarray_create.
 */
void dynarray_destroy(ShmRegion *region, OffsetPtr array_offset);

/**
 * @brief Appends a new element to the end of the array.
 *
 * @param region Shared‑memory region.
 * @param array_offset Offset of the array header.
 * @param element Pointer to the element to copy into the array.
 * @return 0 on success, non‑zero on allocation failure.
 */
int dynarray_push(ShmRegion *region, OffsetPtr array_offset, const void *element);

/**
 * @brief Retrieves an element by index.
 *
 * @param region Shared‑memory region.
 * @param array_offset Offset of the array header.
 * @param index Zero‑based index (must be less than length).
 * @param out   Caller‑provided buffer of at least elem_size bytes.
 * @return 0 on success, non‑zero on out‑of‑range.
 */
int dynarray_get(ShmRegion *region, OffsetPtr array_offset, uint64_t index, void *out);

/**
 * @brief Ensures the array has at least the requested capacity.
 *
 * May allocate a new payload buffer and copy existing data.
 *
 * @param region Shared‑memory region.
 * @param array_offset Offset of the array header.
 * @param new_cap Desired capacity (elements).
 * @return 0 on success, non‑zero on allocation failure.
 */
int dynarray_reserve(ShmRegion *region, OffsetPtr array_offset, uint64_t new_cap);

/**
 * @brief Returns the current number of stored elements.
 */
uint64_t dynarray_length(ShmRegion *region, OffsetPtr array_offset);

#endif /* OFFSET_STORE_DYNARRAY_H */
