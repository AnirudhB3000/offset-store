#include "offset_store/offset_ptr.h"

#include <limits.h>
#include <stdint.h>

OffsetPtr offset_ptr_null(void)
{
    OffsetPtr ptr;

    ptr.offset = 0;
    return ptr;
}

bool offset_ptr_is_null(OffsetPtr ptr)
{
    return ptr.offset == 0;
}

bool offset_ptr_is_in_bounds(size_t region_size, OffsetPtr ptr, size_t span)
{
    uint64_t span_u64;

    /* A zero offset is reserved for null and is never a valid object address. */
    if (offset_ptr_is_null(ptr)) {
        return false;
    }

    if (span > UINT64_MAX) {
        return false;
    }

    span_u64 = (uint64_t) span;
    if (ptr.offset >= (uint64_t) region_size) {
        return false;
    }

    /* Guard the end calculation against integer wraparound. */
    if (span_u64 > ((uint64_t) region_size - ptr.offset)) {
        return false;
    }

    return true;
}

bool offset_ptr_try_from_raw(
    const void *base,
    size_t region_size,
    const void *ptr,
    OffsetPtr *out_ptr
)
{
    uintptr_t base_addr;
    uintptr_t ptr_addr;
    uintptr_t end_addr;
    OffsetPtr converted;

    if (base == NULL || ptr == NULL || out_ptr == NULL) {
        return false;
    }

    base_addr = (uintptr_t) base;
    ptr_addr = (uintptr_t) ptr;
    end_addr = base_addr + region_size;

    /* Reject wraparound and pointers outside the mapped region. */
    if (end_addr < base_addr || ptr_addr < base_addr || ptr_addr >= end_addr) {
        return false;
    }

    converted.offset = (uint64_t) (ptr_addr - base_addr);
    if (converted.offset == 0) {
        return false;
    }

    *out_ptr = converted;
    return true;
}

bool offset_ptr_try_resolve(
    const void *base,
    size_t region_size,
    OffsetPtr ptr,
    size_t span,
    void **out_raw
)
{
    uintptr_t base_addr;

    if (base == NULL || out_raw == NULL) {
        return false;
    }

    if (!offset_ptr_is_in_bounds(region_size, ptr, span)) {
        return false;
    }

    base_addr = (uintptr_t) base;
    *out_raw = (void *) (base_addr + (uintptr_t) ptr.offset);
    return true;
}

bool offset_ptr_try_resolve_const(
    const void *base,
    size_t region_size,
    OffsetPtr ptr,
    size_t span,
    const void **out_raw
)
{
    void *resolved;

    if (out_raw == NULL) {
        return false;
    }

    if (!offset_ptr_try_resolve(base, region_size, ptr, span, &resolved)) {
        return false;
    }

    *out_raw = resolved;
    return true;
}
