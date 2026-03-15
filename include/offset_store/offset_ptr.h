#ifndef OFFSET_STORE_OFFSET_PTR_H
#define OFFSET_STORE_OFFSET_PTR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared-memory references are stored as offsets from the region base. */
typedef struct {
    uint64_t offset;
} OffsetPtr;

/* Returns the canonical null offset pointer. */
OffsetPtr offset_ptr_null(void);

/* Returns true when the offset pointer is the null sentinel. */
bool offset_ptr_is_null(OffsetPtr ptr);

/*
 * Converts a process-local pointer into a shared offset.
 * The conversion succeeds only when ptr lies inside [base, base + region_size).
 */
bool offset_ptr_try_from_raw(
    const void *base,
    size_t region_size,
    const void *ptr,
    OffsetPtr *out_ptr
);

/*
 * Resolves an offset back into a process-local pointer.
 * span is the number of bytes the caller intends to access from the resolved
 * address and is checked against the region bounds.
 */
bool offset_ptr_try_resolve(
    const void *base,
    size_t region_size,
    OffsetPtr ptr,
    size_t span,
    void **out_raw
);

/* Const-qualified version of offset_ptr_try_resolve for read-only callers. */
bool offset_ptr_try_resolve_const(
    const void *base,
    size_t region_size,
    OffsetPtr ptr,
    size_t span,
    const void **out_raw
);

/* Returns true when [ptr.offset, ptr.offset + span) lies within the region. */
bool offset_ptr_is_in_bounds(size_t region_size, OffsetPtr ptr, size_t span);

#endif
