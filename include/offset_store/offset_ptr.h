#ifndef OFFSET_STORE_OFFSET_PTR_H
#define OFFSET_STORE_OFFSET_PTR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file offset_ptr.h
 * @brief Offset-based shared-memory pointer conversion and resolution helpers.
 */

/**
 * @name Shared-Memory Resident Value Types
 *
 * These types are safe to embed inside shared-memory-resident metadata because
 * they are defined in terms of offsets or fixed-width scalar fields rather than
 * process-local addresses.
 */
/**@{*/

/**
 * @brief Address-independent shared-memory reference.
 *
 * Shared-memory references are stored as offsets from the region base so the
 * same on-disk or in-memory layout can be resolved by processes with different
 * virtual addresses.
 */
typedef struct {
    /** Offset from the shared-memory base address. Zero is the null sentinel. */
    uint64_t offset;
} OffsetPtr;

/**@}*/

/**
 * @name Offset Pointer Helpers
 * @{
 */

/**
 * @brief Returns the canonical null offset pointer.
 *
 * @return Offset pointer whose offset is zero.
 */
OffsetPtr offset_ptr_null(void);

/**
 * @brief Returns whether an offset pointer is the null sentinel.
 *
 * @param ptr Offset pointer to inspect.
 * @return true if @p ptr is null.
 * @return false otherwise.
 */
bool offset_ptr_is_null(OffsetPtr ptr);

/**
 * @brief Converts a process-local pointer into a shared offset.
 *
 * The conversion succeeds only when @p ptr lies within the mapped region and
 * does not resolve to offset zero.
 *
 * @param base Base address of the mapped region.
 * @param region_size Size of the mapped region in bytes.
 * @param ptr Process-local pointer to convert.
 * @param[out] out_ptr Converted offset pointer on success.
 * @return true if conversion succeeds.
 * @return false if arguments are invalid or @p ptr lies outside the region.
 */
bool offset_ptr_try_from_raw(
    const void *base,
    size_t region_size,
    const void *ptr,
    OffsetPtr *out_ptr
);

/**
 * @brief Resolves an offset back into a process-local pointer.
 *
 * @param base Base address of the mapped region.
 * @param region_size Size of the mapped region in bytes.
 * @param ptr Offset pointer to resolve.
 * @param span Number of bytes the caller intends to access from the resolved address.
 * @param[out] out_raw Resolved process-local pointer on success.
 * @return true if resolution succeeds and the requested span stays in bounds.
 * @return false otherwise.
 */
bool offset_ptr_try_resolve(
    const void *base,
    size_t region_size,
    OffsetPtr ptr,
    size_t span,
    void **out_raw
);

/**
 * @brief Resolves an offset for read-only callers.
 *
 * @param base Base address of the mapped region.
 * @param region_size Size of the mapped region in bytes.
 * @param ptr Offset pointer to resolve.
 * @param span Number of bytes the caller intends to access from the resolved address.
 * @param[out] out_raw Resolved const process-local pointer on success.
 * @return true if resolution succeeds and the requested span stays in bounds.
 * @return false otherwise.
 */
bool offset_ptr_try_resolve_const(
    const void *base,
    size_t region_size,
    OffsetPtr ptr,
    size_t span,
    const void **out_raw
);

/**
 * @brief Returns whether an offset span lies entirely within the region.
 *
 * @param region_size Size of the mapped region in bytes.
 * @param ptr Offset pointer describing the start of the span.
 * @param span Requested span size in bytes.
 * @return true if the span is valid and in bounds.
 * @return false otherwise.
 */
bool offset_ptr_is_in_bounds(size_t region_size, OffsetPtr ptr, size_t span);

/** @} */

#endif
