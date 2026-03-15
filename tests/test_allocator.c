#define _POSIX_C_SOURCE 200809L

#include "offset_store/allocator.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void make_region_name(char *buffer, size_t buffer_size, const char *suffix)
{
    /*
     * Shared-memory names are global on the host, so tests include the PID to
     * isolate parallel or repeated runs.
     */
    int written;

    written = snprintf(buffer, buffer_size, "/offset-store-%ld-%s", (long) getpid(), suffix);
    assert(written > 0);
    assert((size_t) written < buffer_size);
}

static void test_allocator_init_and_validate(void)
{
    char name[64];
    ShmRegion region;
    const AllocatorHeader *header;

    make_region_name(name, sizeof(name), "alloc-init");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&region, name, 4096));
    assert(allocator_init(&region));
    assert(allocator_validate(&region));

    header = allocator_header(&region);
    assert(header != NULL);
    assert(header->free_list_head.offset == header->heap_offset);

    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

static void test_allocator_alloc_and_free_round_trip(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;
    void *reused;

    make_region_name(name, sizeof(name), "alloc-free");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&region, name, 4096));
    assert(allocator_init(&region));

    assert(allocator_alloc(&region, 64, 16, &allocation));
    memset(allocation, 0x5a, 64);
    assert(allocator_validate(&region));

    assert(allocator_free(&region, allocation));
    assert(allocator_validate(&region));

    assert(allocator_alloc(&region, 64, 16, &reused));
    assert(reused == allocation);

    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

static void test_allocator_honors_alignment(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;

    make_region_name(name, sizeof(name), "alloc-align");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&region, name, 4096));
    assert(allocator_init(&region));
    assert(allocator_alloc(&region, 32, 64, &allocation));
    assert(((uintptr_t) allocation % 64u) == 0);

    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

static void test_allocator_rejects_double_free(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;

    make_region_name(name, sizeof(name), "alloc-double-free");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&region, name, 4096));
    assert(allocator_init(&region));
    assert(allocator_alloc(&region, 48, 16, &allocation));
    assert(allocator_free(&region, allocation));
    assert(!allocator_free(&region, allocation));
    assert(errno == EINVAL);

    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

static void test_allocator_state_is_visible_after_attach(void)
{
    char name[64];
    ShmRegion creator_region;
    ShmRegion attached_region;
    const AllocatorHeader *creator_header;
    const AllocatorHeader *attached_header;
    void *allocation;

    make_region_name(name, sizeof(name), "alloc-attach");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&creator_region, name, 4096));
    assert(allocator_init(&creator_region));
    assert(allocator_alloc(&creator_region, 80, 16, &allocation));

    assert(shm_region_open(&attached_region, name));
    assert(allocator_validate(&attached_region));

    creator_header = allocator_header(&creator_region);
    attached_header = allocator_header(&attached_region);
    assert(creator_header != NULL);
    assert(attached_header != NULL);
    assert(creator_header->heap_offset == attached_header->heap_offset);
    assert(creator_header->heap_size == attached_header->heap_size);
    assert(creator_header->free_list_head.offset == attached_header->free_list_head.offset);

    assert(shm_region_close(&attached_region));
    assert(shm_region_close(&creator_region));
    assert(shm_region_unlink(name));
}

static void test_allocator_rejects_invalid_alignment(void)
{
    char name[64];
    ShmRegion region;
    void *allocation;

    make_region_name(name, sizeof(name), "alloc-align-invalid");
    assert(!shm_region_unlink(name));

    assert(shm_region_create(&region, name, 4096));
    assert(allocator_init(&region));
    assert(!allocator_alloc(&region, 32, 24, &allocation));
    assert(errno == EINVAL);

    assert(shm_region_close(&region));
    assert(shm_region_unlink(name));
}

int main(void)
{
    test_allocator_init_and_validate();
    test_allocator_alloc_and_free_round_trip();
    test_allocator_honors_alignment();
    test_allocator_rejects_double_free();
    test_allocator_state_is_visible_after_attach();
    test_allocator_rejects_invalid_alignment();
    return 0;
}
