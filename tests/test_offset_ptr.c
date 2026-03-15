#include "offset_store/offset_ptr.h"

#include <assert.h>
#include <stdint.h>

/*
 * These tests exercise the rules that the rest of the project will depend on:
 * zero offset is null, raw pointers must stay within the region, and resolved
 * spans must not cross region bounds.
 */
static void test_null_offset_pointer(void)
{
    OffsetPtr ptr;

    ptr = offset_ptr_null();
    assert(offset_ptr_is_null(ptr));
    assert(!offset_ptr_is_in_bounds(64, ptr, 1));
}

static void test_round_trip_conversion(void)
{
    uint8_t region[64] = {0};
    OffsetPtr ptr;
    void *resolved;

    assert(offset_ptr_try_from_raw(region, sizeof(region), &region[8], &ptr));
    assert(ptr.offset == 8);
    assert(offset_ptr_try_resolve(region, sizeof(region), ptr, 4, &resolved));
    assert(resolved == &region[8]);
}

static void test_rejects_base_pointer_as_storable_reference(void)
{
    uint8_t region[32] = {0};
    OffsetPtr ptr;

    assert(!offset_ptr_try_from_raw(region, sizeof(region), region, &ptr));
}

static void test_rejects_pointer_outside_region(void)
{
    uint8_t region[32] = {0};
    uint8_t outside;
    OffsetPtr ptr;

    outside = 0;
    assert(!offset_ptr_try_from_raw(region, sizeof(region), &outside, &ptr));
}

static void test_span_validation(void)
{
    uint8_t region[32] = {0};
    OffsetPtr ptr;
    void *resolved;

    assert(offset_ptr_try_from_raw(region, sizeof(region), &region[28], &ptr));
    assert(offset_ptr_is_in_bounds(sizeof(region), ptr, 4));
    assert(!offset_ptr_is_in_bounds(sizeof(region), ptr, 5));
    assert(offset_ptr_try_resolve(region, sizeof(region), ptr, 4, &resolved));
    assert(!offset_ptr_try_resolve(region, sizeof(region), ptr, 5, &resolved));
}

static void test_const_resolution(void)
{
    uint8_t region[16] = {0};
    OffsetPtr ptr;
    const void *resolved;

    assert(offset_ptr_try_from_raw(region, sizeof(region), &region[4], &ptr));
    assert(offset_ptr_try_resolve_const(region, sizeof(region), ptr, 2, &resolved));
    assert(resolved == &region[4]);
}

int main(void)
{
    test_null_offset_pointer();
    test_round_trip_conversion();
    test_rejects_base_pointer_as_storable_reference();
    test_rejects_pointer_outside_region();
    test_span_validation();
    test_const_resolution();
    return 0;
}
