# Source Walkthrough

This directory contains the concrete implementation of the shared-memory object
store. The top-level [`README.md`](/home/aniru/offset-store/README.md) explains
the public model and project goals. This document explains how the current
implementation works module by module, with emphasis on the allocator and object
store.

## Module Map

- `offset_store.c`: library-wide public status strings
- `shm_region.c`: POSIX shared-memory lifecycle and the shared region header
- `offset_ptr.c`: offset conversion and bounds-checked resolution helpers
- `allocator.c`: in-region free-list allocator
- `object_store.c`: fixed-header objects built on allocator allocations
- `store.c`: convenience wrapper for bootstrap/open/close flows

## `shm_region.c`

`shm_region.c` owns the outer shared-memory container. Its job is to create or
open a POSIX shared-memory object, map it into the current process, and manage a
small private header stored at offset `0` in the mapping.

The private `ShmRegionHeader` currently contains:

```c
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    uint64_t total_size;
    pthread_mutex_t allocator_mutex;
    pthread_rwlock_t roots_rwlock;
    pthread_rwlock_t index_rwlock;
    ShmRegionRootEntry roots[OFFSET_STORE_ROOT_CAPACITY];
    ShmRegionIndexEntry index[OFFSET_STORE_INDEX_CAPACITY];
} ShmRegionHeader;
```

Important points:

- the header lives inside shared memory and is therefore visible to all
  attached processes
- the public `ShmRegion` struct is process-local and must never be stored in the
  mapping
- the allocator mutex and roots/index rwlocks are configured with `PTHREAD_PROCESS_SHARED`
- allocator mutation uses `allocator_mutex`
- root-table operations use `roots_rwlock`
- shared-index operations use `index_rwlock`
- the header also contains a fixed-capacity inline root table for discovering
  well-known shared objects by stable names
- the header also contains a fixed-capacity inline index table for general
  key-to-object discovery without allocator recursion
- attach paths validate `magic`, `version`, and `total_size` before the region
  is considered usable
- callers now have both `shm_region_data(...)` and `shm_region_data_const(...)`
  so read-only access does not require casting away const intent
- `shm_region_validate(...)` now exposes the private header integrity check as a
  public status-returning helper
- `shm_region_validate(...)` also verifies that the allocator mutex and the
  roots/index rwlocks are operational
- if a future operation ever needs more than one subsystem lock, the canonical
  order is allocator, then roots, then index

This module deliberately keeps the shared header typedef private so callers
cannot accidentally depend on its binary layout in public code.

## `offset_ptr.c`

`offset_ptr.c` centralizes the rule that shared references are offsets from the
region base, never raw pointers.

The rules enforced here are simple but foundational:

- `OffsetPtr{ .offset = 0 }` is the null sentinel
- a storable offset must point somewhere inside the mapped region
- resolution checks both the starting offset and the requested access span
- conversions reject the mapping base itself so offset zero remains reserved for
  null

Every higher-level module depends on these helpers to convert between
process-local pointers and address-independent shared references.

## `allocator.c`

`allocator.c` is the shared heap manager. All allocator metadata lives inside the
shared mapping immediately after the region header. Nothing in the allocator
depends on process-local addresses being stable across attaches.

### Shared Layout

The allocator stores a private `AllocatorHeader` after the region header:

```c
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t reserved;
    uint64_t heap_offset;
    uint64_t heap_size;
    OffsetPtr free_list_head;
    uint64_t allocation_failures;
} AllocatorHeader;
```

The heap begins at `heap_offset`, aligned to `max_align_t`. The allocator then
walks the heap as a physical sequence of variable-sized blocks.

Each heap block starts with:

```c
typedef struct {
    uint64_t size;
    OffsetPtr next_free;
    uint32_t flags;
    uint32_t payload_offset;
} AllocatorBlockHeader;
```

Meaning of the fields:

- `size`: total block size, including header and payload area
- `next_free`: free-list link when the block is free
- `flags`: `OFFSET_STORE_ALLOCATOR_BLOCK_FREE` or `0`
- `payload_offset`: byte offset from block start to the live allocation payload

### Initialization

`allocator_init(...)` computes the aligned heap start from the mapped region
size, writes the allocator header, and creates one initial free block that spans
the entire heap. Initialization is currently strict one-shot behavior:

- a valid existing allocator header causes `OFFSET_STORE_STATUS_ALREADY_EXISTS`
- an undersized region causes `OFFSET_STORE_STATUS_OUT_OF_MEMORY`

The function takes the allocator mutex internally before mutating shared
allocator state.

### Allocation Path

`allocator_alloc(...)` implements a first-fit walk over the free list.

For each free block it:

1. Computes the earliest possible payload start that leaves room for the block
   header and an in-band allocation prefix.
2. Aligns that payload start to the requested alignment.
3. Rounds the resulting allocation size up to `max_align_t` so the physical heap
   remains traversable.
4. Checks whether the block is large enough.
5. Either splits the block or consumes it whole.

Immediately before the returned payload, the allocator writes a private prefix:

```c
typedef struct {
    uint64_t block_offset;
} AllocationPrefix;
```

That prefix is the key to keeping `free(...)` address-independent. The allocator
does not need to store a raw pointer to the owning block; it recovers the block
later by reading the stored offset and resolving it relative to the current
mapping base.

If no free block can satisfy the request, the allocator increments the persistent
`allocation_failures` counter in `AllocatorHeader` before returning
`OFFSET_STORE_STATUS_OUT_OF_MEMORY`. Invalid-argument failures do not affect that
counter.

### Free Path

`allocator_free(...)` reads the prefix just before the caller's payload pointer,
reconstructs the owning block offset, validates that the block lives inside the
heap and that the payload address matches `payload_offset`, then puts the block
back at the head of the free list.

The current implementation does not coalesce adjacent free blocks. That keeps
the logic simple, but it also means fragmentation is possible under mixed-size
allocation patterns.

### Validation

`allocator_validate(...)` performs two structural checks:

- a linear physical walk across the heap to verify block sizes and flags
- a free-list walk to verify that free-list links point to in-heap free blocks

This is a debugging and attach-time integrity check, not a crash-recovery
protocol. It catches basic corruption and layout mistakes, but it does not yet
repair damage.

### Locking

The allocator currently acquires the allocator mutex internally in:

- `allocator_init(...)`
- `allocator_alloc(...)`
- `allocator_free(...)`

Read-mostly helpers such as `allocator_validate(...)` and the metadata query
functions do not currently lock. They assume callers either tolerate a
best-effort snapshot or arrange higher-level synchronization when concurrent
mutation matters.

The same category now includes `allocator_get_stats(...)`, which derives
heap-usage and fragmentation numbers by walking the current block sequence at
call time instead of maintaining persistent counters in shared metadata.

The same rule extends to object-store accessors. Functions such as
`object_store_get_header(...)`, `object_store_get_header_mut(...)`,
`object_store_get_payload_const(...)`, and `object_store_get_payload(...)`
perform validation but do not acquire the allocator mutex themselves.

## `object_store.c`

`object_store.c` layers a stable object format on top of allocator allocations.
An object is just one allocator allocation whose first bytes contain a fixed
header:

```c
typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t flags;
    uint32_t reserved;
} ObjectHeader;
```

The object handle returned to callers is an `OffsetPtr` pointing to the start of
that header. The payload immediately follows it:

```text
[ObjectHeader][payload bytes]
```

### Allocation Flow

`object_store_alloc(...)`:

1. asks the allocator for `sizeof(ObjectHeader) + payload_size` bytes
2. writes the object header into the returned storage
3. converts the header address into an `OffsetPtr`
4. returns that offset as the stable shared-memory handle

This preserves the main project invariant: the shared handle is relocatable
across processes because it stores an offset rather than a raw address.

### Resolution Flow

Object resolution goes through `object_store_resolve_header(...)`, which checks:

- the object offset is non-null
- the requested header-plus-payload span stays inside the mapped region
- the resolved address matches the start of a live allocator allocation

That last check matters. It prevents arbitrary in-range offsets from being
treated as valid objects just because they point somewhere inside the mapping.

Public helpers are thin wrappers around that resolution:

- `object_store_get_header(...)`
- `object_store_get_header_mut(...)`
- `object_store_get_payload_const(...)`
- `object_store_get_payload(...)`
- `object_store_validate(...)`

### Free Flow

`object_store_free(...)` resolves the object header, verifies that the full
object span is still valid, and then passes the header pointer back to
`allocator_free(...)`.

Before releasing the block, the object store now marks the header with the
library-owned `OFFSET_STORE_OBJECT_FLAG_FREED` bit and poisons selected header
fields. That gives stale-handle reads a clearer failure mode: object accessors
and `object_store_validate(...)` reject freed objects explicitly instead of
silently depending only on allocator ownership checks.

The object store does not add a second free list or object registry. It relies
completely on allocator ownership and offset validation.

### Concurrency Notes

Object allocation and object free inherit allocator locking because those paths
ultimately call allocator mutation functions. Pure reads through the object
accessors do not acquire the mutex.

That means the current consistency model is simple but limited:

- allocation and free are serialized by the allocator mutex
- object reads are only as stable as the caller's external synchronization
- concurrent mutation of an object's payload is a caller-level concern today
- concurrent allocator mutation or object free can invalidate a previously
  resolved process-local pointer if the caller does not hold external
  synchronization

## `store.c`

`store.c` provides the ergonomic wrapper around the lower-level modules:

- `offset_store_bootstrap(...)`: create region, map it, initialize allocator
- `offset_store_open_existing(...)`: attach to region, validate allocator state
- `offset_store_close(...)`: close the mapping
- `offset_store_validate(...)`: validate the region header plus allocator state
- `offset_store_set_root(...)`: store or replace a named root binding
- `offset_store_get_root(...)`: resolve a named root to its stored object handle
- `offset_store_remove_root(...)`: delete a named root binding
- `offset_store_index_put(...)`: store or replace a general index binding
- `offset_store_index_get(...)`: resolve a general index key to its stored handle
- `offset_store_index_contains(...)`: check whether an index key is present
- `offset_store_index_remove(...)`: delete a general index binding

`OffsetStore` is process-local convenience state, not a shared-memory structure.
It exists to reduce boilerplate in examples and common lifecycle code.

Current store-level flows remain single-subsystem:

- bootstrap initializes the region header and then takes only the allocator lock
- open/validate check region and allocator state without nested subsystem locks
- root and index publication helpers each stay within their own subsystem lock

For recommended caller-side sequencing and error-handling patterns, see the
`API Usage` section in [`README.md`](/home/aniru/offset-store/README.md).

## Current Limits

The current implementation is intentionally conservative:

- one allocator mutex plus rwlocks for the roots and index subsystems
- first-fit allocation
- no free-block coalescing
- no crash recovery journal
- no robust handling for the allocator mutex or the roots/index rwlocks after process death

Those tradeoffs keep the layout deterministic and the code inspectable, which is
consistent with the repository's stated learning and experimentation goals.
