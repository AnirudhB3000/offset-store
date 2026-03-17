# Memory Layout

This document describes the current `v0.1.1` shared-memory layout implemented in
the repository. It focuses on the actual bytes stored in the shared region rather
than the process-local helper structures used to access them.

Interpret the structure snippets in this document as layout documentation first.
Some of the types shown here are public API types with stable exposed layouts,
while others are private implementation structs that remain intentionally hidden
from the public headers.

Public versus private boundary:

- public shared-memory value types: `OffsetPtr`, `ObjectHeader`
- public process-local descriptors: `ShmRegion`, `OffsetStore`
- private shared-memory layout structs: `ShmRegionHeader`, `AllocatorHeader`,
  `AllocatorBlockHeader`, `AllocationPrefix`

Only the shared-memory value types are intended to be embedded in allocator-owned
objects. Process-local descriptors and private implementation structs are not
part of the stable caller-facing data model.

## Region Overview

The mapped shared-memory region is divided into three major areas:

```text
+------------------------------+
| ShmRegionHeader              |
+------------------------------+
| AllocatorHeader              |
+------------------------------+
| Heap blocks                  |
+------------------------------+
```

The region header and allocator header occupy fixed positions at the front of the
mapping. The remainder of the region is the allocator-managed heap.

## Region Header

The region header is stored at offset `0` from the region base. Its typedef is
kept private to `src/shm_region.c`, but the current byte layout is:

```c
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    uint64_t total_size;
    pthread_mutex_t mutex;
} ShmRegionHeader;
```

Fields:

- `magic`: identifies the mapping as an offset-store region
- `version`: region layout version
- `reserved`: padding/reserved space for future use
- `total_size`: full mapped region size in bytes
- `mutex`: process-shared mutex protecting coarse-grained shared mutation

Behavior:

- initialized by `shm_region_create`
- validated by `shm_region_open`
- used by allocator mutation paths through `shm_region_lock` and
  `shm_region_unlock`

Failure notes:

- if the header is corrupted, `shm_region_open` rejects the mapping
- the mutex is process-shared but not yet configured as robust, so crash recovery
  is still a future concern

## Allocator Header

The allocator stores an internal header immediately after `ShmRegionHeader`:

```c
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    uint64_t heap_offset;
    uint64_t heap_size;
    OffsetPtr free_list_head;
} AllocatorHeader;
```

Fields:

- `magic`: identifies initialized allocator metadata
- `version`: allocator layout version
- `reserved`: reserved for future use
- `heap_offset`: byte offset from region base to the first heap block
- `heap_size`: total bytes available in the heap area
- `free_list_head`: offset to the first free block, or null if no free blocks remain

Behavior:

- initialized by `allocator_init`
- remains entirely in shared memory
- points into the heap using `OffsetPtr`, never raw pointers

## Heap Layout

The heap begins at `heap_offset`, which is aligned to `max_align_t`.

The heap is a sequence of variable-sized blocks:

```text
+---------------------------+
| AllocatorBlockHeader      |
+---------------------------+
| allocation prefix         |
+---------------------------+
| aligned payload bytes     |
+---------------------------+
| optional remainder block  |
+---------------------------+
```

The current implementation supports splitting a free block when the remainder is
large enough to form another valid block.

### Block Header

Each block starts with an internal block header containing:

Fields:

- `size`: total block size in bytes, including header and payload area
- `next_free`: free-list link when the block is free
- `flags`: currently `OFFSET_STORE_ALLOCATOR_BLOCK_FREE` or `0`
- `payload_offset`: byte offset from the block start to the payload start for live allocations

Rules:

- all free-list links use offsets
- allocated blocks keep the same header layout for deterministic traversal
- `payload_offset` is meaningful only for live allocations

### Allocation Prefix

Immediately before each allocated payload is an in-band prefix:

```c
typedef struct {
    uint64_t block_offset;
} AllocationPrefix;
```

This prefix is not a public API structure. It stores the owning block offset so
`allocator_free` can recover the block without storing raw pointers in shared
memory metadata.

## Object Layout

Objects are allocated from the allocator as ordinary payload blocks whose first
bytes contain an `ObjectHeader`:

```c
typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t flags;
    uint32_t reserved;
} ObjectHeader;
```

Actual object memory layout:

```text
[ObjectHeader][Object payload bytes]
```

Rules:

- object handles are `OffsetPtr` values pointing to the start of `ObjectHeader`
- object payload begins immediately after the header
- object resolution validates both region bounds and allocator ownership

## Process-Local Structures

Some structs are intentionally process-local and are not stored in shared memory.

`ShmRegion` is one example:

```c
typedef struct {
    int fd;
    void *base;
    size_t size;
    bool creator;
} ShmRegion;
```

This descriptor tracks the local file descriptor and mapping address for one
process. It must never be embedded in shared-memory-resident structures.

The public `shm_region` API now exposes region metadata through query helpers
instead of returning a typed pointer to the shared header. That keeps shared
layout details documented here without making them part of the stable public C
surface.

The same separation applies across the public headers:

- process-local descriptors such as `ShmRegion` and `OffsetStore` are not part
  of the shared-memory layout
- shared-memory-resident value types such as `OffsetPtr` and `ObjectHeader` are
  safe to persist in allocator-owned storage
- private shared-memory layout structs remain implementation details of the
  corresponding `.c` files

Practical rule:

- persist `OffsetPtr` values, object payload bytes, and any higher-level structs
  composed from stable shared-memory value types
- do not persist resolved raw pointers returned by accessors
- do not persist `ShmRegion`, `OffsetStore`, or any private header/block struct

For caller-side sequencing and error handling, see the `API Usage` section in
[`README.md`](/home/aniru/offset-store/README.md).

## Current Locking Scope

The current mutex scope is coarse-grained:

- `allocator_init`
- `allocator_alloc`
- `allocator_free`

The following public APIs do not lock internally:

- `allocator_validate(...)`
- allocator metadata query helpers
- `object_store_get_header(...)`
- `object_store_get_header_mut(...)`
- `object_store_get_payload_const(...)`
- `object_store_get_payload(...)`
- `shm_region` metadata/query helpers other than `shm_region_lock(...)` and
  `shm_region_unlock(...)`

That means callers must arrange external synchronization if they need a stable
view while another process might allocate, free, or mutate shared objects.

This keeps the design simple for the current implementation. Future versions may
replace it with a more granular design once the object graph and indexing layers
exist.
