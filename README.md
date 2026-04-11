# Offset Store

`offset-store` is a shared memory object store built around one central constraint:
processes do not share a stable virtual address space, so objects in shared memory
cannot store raw pointers.

Instead, all references inside the shared region are represented as offsets from the
base address returned by `mmap`. This makes the same in-memory layout valid across
multiple processes even when each process maps the region at a different address.

This repository is intended as a systems project focused on simple, deterministic,
well-documented building blocks for:

- shared memory region management
- offset-based object references
- custom allocation within the shared region
- multi-process synchronization
- stable object layout rules
- experimentation with crash tolerance and recovery

For a source-level walkthrough of the implemented modules, especially the
allocator and object-store internals, see
[`src/README.md`](/home/aniru/offset-store/src/README.md).

Code documentation now follows a Doxygen-style convention for public headers,
implementation helpers, tests, and examples. Public headers also use grouped
`@name ... @{ ... @}` sections so related APIs appear together in both the
source and generated documentation. The same grouping style now also applies to
the main implementation `.c` files so private layout structs, internal helpers,
and exported entry points are sectioned consistently. The Unity test files now
follow the same convention, grouping lifecycle hooks, shared helpers, worker
helpers, test cases, and test runners with Doxygen sections.

The allocator mutex is configured as a robust process-shared mutex on
platforms that support `pthread_mutexattr_setrobust(...)`. If a process dies
while holding that mutex, the next allocator lock attempt returns
`OFFSET_STORE_STATUS_INVALID_STATE` after marking the mutex consistent and
releasing it so callers can validate or repair shared state before retrying.
The roots and index rwlocks remain non-robust because portable POSIX rwlock
owner-death recovery is not available.

The private shared region header also carries stable metadata: a region 
state-flags field, a generation counter, and a checksum over the stable header 
bytes plus the inline root/index tables. Attach and validation paths reject 
regions that are not fully ready or whose stable metadata has been corrupted.

The allocator uses sharded locking: the heap is partitioned into N shards
(currently 4), each with its own process-shared mutex and free list. This reduces
contention under high-load multi-process scenarios by allowing concurrent allocations
from different shards. Shard selection uses size-based routing to improve locality.
Allocation failures are tracked atomically in shared metadata using `_Atomic`
operations with `memory_order_relaxed` for lock-free performance.

The polished getter/accessor APIs now also have explicit regression coverage in
the unit tests so naming and contract behavior stay stable during future cleanup.

At the time of writing, the repository contains project instructions, top-level
documentation, initial repository scaffolding, and the first implemented modules:
`offset_ptr`, `shm_region`, `allocator`, `object_store`, and the higher-level
`OffsetStore` lifecycle wrapper. This README documents both the intended
architecture and the concrete APIs that now exist so implementation work can
stay aligned with a consistent model.

## Design Goals

The project is designed around these goals:

- deterministic memory layout across processes
- no absolute pointers stored in shared memory
- allocator metadata stored inside the shared region
- process-safe concurrent access
- minimal hidden behavior outside the shared allocator
- code that remains easy to inspect, debug, and extend

The project explicitly favors clarity and correctness over premature optimization.

## Core Problem

If two processes attach to the same POSIX shared memory object, each process may see
the region at a different base address:

```text
process A: 0x700000000000
process B: 0x500000000000
```

If an object stores a raw pointer such as `0x700000001230`, that pointer is only valid
in process A. The same numeric address in process B may be unmapped or point to an
unrelated location.

Because of that, raw pointers are forbidden in any shared-memory-resident structure.

The solution is to store:

```text
offset = object_address - shared_memory_base
```

Then each process resolves the object locally:

```text
resolved_address = shared_memory_base + offset
```

This makes references relocatable across processes.

## Shared Memory Model

All allocator metadata, object metadata, and object storage live inside one shared
memory region.

Typical lifecycle:

1. Open or create a shared memory object with `shm_open`.
2. Set the region size with `ftruncate`.
3. Map the region with `mmap`.
4. Initialize metadata inside the mapped region if this is the first creator.
5. Allow multiple processes to attach and operate on the same region.

Conceptual layout:

```text
+------------------------------+
| region header                |
+------------------------------+
| synchronization primitives   |
+------------------------------+
| allocator metadata           |
+------------------------------+
| object heap                  |
+------------------------------+
| free blocks / free lists     |
+------------------------------+
```

The exact layout should remain deterministic.

Current implemented `shm_region` responsibilities:

- create a new shared memory object and size it with `ftruncate`
- map a region with `mmap`
- store and validate a fixed private region header at the start of the mapping
- record region state flags, a generation counter, and a stable-header checksum
- initialize and expose the process-shared allocator mutex plus the roots/index rwlocks stored in the region header
- store a fixed-capacity named root table for well-known shared objects
- store a fixed-capacity shared index table for general key-to-object discovery
- expose region metadata through narrow query helpers plus data start and usable
  payload size
- expose a public region-header validation entry point
- close mappings and unlink shared memory objects

Public type boundary:

- `ShmRegion` and `OffsetStore` are process-local descriptors and must never be
  embedded in shared-memory-resident structures
- `OffsetPtr` and `ObjectHeader` are stable value types intended for
  shared-memory-resident layouts
- private shared-memory layout structs such as `ShmRegionHeader` and
  `AllocatorHeader` remain intentionally hidden in `.c` files

The corresponding byte-level layout notes, including which documented structs are
public versus private implementation details, are in
[`docs/memory-layout.md`](/home/aniru/offset-store/docs/memory-layout.md).

## Offset Pointer Model

The fundamental shared reference type is an offset pointer.

Example conceptual layout:

```c
typedef struct {
    uint64_t offset;
} OffsetPtr;
```

Rules:

- offsets are always relative to the shared memory base
- `offset == 0` means null
- only offsets are stored in shared memory structures
- conversion to raw pointers happens transiently in process-local code
- resolved pointers must be bounds-checked in debug or validation paths

Current implemented helper operations:

```c
typedef struct {
    uint64_t offset;
} OffsetPtr;

OffsetPtr offset_ptr_null(void);
bool offset_ptr_is_null(OffsetPtr ptr);
bool offset_ptr_try_from_raw(
    const void *base,
    size_t region_size,
    const void *ptr,
    OffsetPtr *out_ptr
);
bool offset_ptr_try_resolve(
    const void *base,
    size_t region_size,
    OffsetPtr ptr,
    size_t span,
    void **out_raw
);
```

Required safety behavior:

- reject or flag offsets outside the mapped region
- reject offset `0` as a storable object reference
- preserve structure layout consistency across compiler settings
- avoid storing host-process-only addresses in persistent metadata

Algorithmically, offset handling is simple:

1. To store a reference, subtract the region base from a process-local pointer.
2. Reject the result if it is zero or falls outside the mapped region.
3. Persist only the resulting integer offset.
4. To resolve a reference, add the stored offset back to the current process's
   region base.
5. Reject the resolution if the requested access span would run past the mapping.

That algorithm is the foundation for every higher-level structure in the
repository. The allocator, object store, and root table all depend on the fact
that an integer offset survives attach-at-a-different-address while a raw
pointer does not.

## Allocator Architecture

The shared heap uses a custom allocator that operates entirely inside the shared
memory segment.

Allocator requirements:

- allocator metadata must live inside shared memory
- allocations return addresses that can be converted to offsets
- freed blocks must re-enter shared free structures safely
- the allocator must behave correctly across multiple attached processes
- objects should not move after allocation unless compaction is deliberately designed

Important implication:

If an object stores references to other objects, those references must also be encoded
as offsets, never as direct pointers returned by the allocator.

Current implemented allocator behavior:

- allocator metadata is stored in-region immediately after the region header
- the heap begins after the allocator header, aligned to `max_align_t`
- free space is tracked as a first-fit singly linked free list using `OffsetPtr`
- each block begins with a stable shared-memory block header
- the allocator uses sharded locking: the heap is partitioned into N shards
  (currently 4), each with its own process-shared mutex and free list
- shard selection uses size-based routing to improve locality for similar-sized
  allocations
- allocation requests fall back through shards if the preferred shard has no
  suitable block
- each shard tracks its own free list independently
- allocation failures increment a persistent `_Atomic` counter in shared metadata
  using lock-free `memory_order_relaxed` atomics
- allocation supports caller-specified power-of-two alignment
- freeing reconstructs the owning block from an in-band prefix that stores the
  block offset rather than a raw pointer
- `allocator_validate` walks the physical heap and each shard's free list to
  catch basic structural corruption
- allocator metadata layout remains internal to `src/allocator.c`; the public API
  now exposes small query helpers instead of the raw allocator header struct
- `allocator_get_stats(...)` provides a snapshot of heap usage and free-space
  fragmentation by walking the current block layout at call time
- allocator metadata now also tracks a cumulative `allocation_failures` counter
  for requests that could not be satisfied from the current free space

Allocator algorithm:

1. `allocator_init(...)` computes where the allocator header ends, rounds the
   heap start up to `max_align_t`, and creates one initial free block covering
   the remaining bytes for each shard. The heap is divided evenly among shards.
2. `allocator_alloc(...)` selects a preferred shard based on allocation size
   using a hash-based distribution, then walks that shard's free list in first-fit order.
3. If the preferred shard cannot satisfy the request, the allocator falls back
   to trying other shards in round-robin fashion.
4. For each candidate block, it computes where a payload could start after the
   block header and allocation prefix, then rounds that payload start up to the
   caller's requested alignment.
5. If the block is large enough, the allocator either consumes the whole block
   or splits it into an allocated block plus a remainder free block that remains
   in the same shard.
6. Immediately before the returned payload, the allocator writes a small prefix
   containing the owning block offset.
7. `allocator_free(...)` reads that prefix back, reconstructs the block, checks
   that it still looks like a live allocation, and pushes it onto the free list
   for the block's shard.

This is intentionally a simple first-fit allocator with sharded free lists.
It does not currently coalesce adjacent free blocks, so fragmentation is possible
after mixed-size allocation patterns.

## Object Layout

Objects stored in the shared heap should begin with a stable header.

Conceptual example:

```c
typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t flags;
    uint32_t reserved;
} ObjectHeader;
```

Then the object data follows immediately after the header:

```text
[ObjectHeader][Object payload]
```

Why this matters:

- generic traversal and validation become possible
- type-aware code can distinguish object classes
- recovery or debugging tools can inspect heap contents
- corruption detection can use size and type metadata

Current implemented object-store behavior:

- objects are allocated from the shared allocator
- the returned object handle is an `OffsetPtr` to the object header
- the payload follows the fixed 16-byte `ObjectHeader`
- header and payload resolution helpers validate both region bounds and allocator
  ownership before returning pointers
- freeing an object releases the underlying allocator block
- `object_store_validate(...)` provides explicit per-handle validation for live
  objects without requiring callers to infer validity from a failed accessor
- one object-header flag bit is now reserved by the library to mark freed
  objects and reject stale-handle reuse more explicitly

Object-store algorithm:

1. `object_store_alloc(...)` asks the allocator for enough bytes to hold
   `ObjectHeader` plus the requested payload.
2. It writes the fixed header at the start of that allocation and converts the
   header address into an `OffsetPtr`.
3. That `OffsetPtr` becomes the stable shared-memory handle for the object.
4. Accessors resolve the handle back to a process-local pointer, verify that it
   points to the start of a live allocator allocation, and confirm that the
   header-declared payload fits within that allocation.
5. `object_store_free(...)` resolves the same handle, marks the header as
   freed, poisons selected fields for debugging, and then releases the
   underlying allocation back to the allocator.

The important detail is that the object store does not own a second heap or
registry. It is a disciplined layer on top of the shared allocator.

## Shared-Memory Containers

The library now provides two offset-based container types for storing collections
in shared memory:

### Dynamic Array (vector)

`dynarray` provides a resizable array stored entirely in shared memory. It is
implemented as a generic object with a header containing capacity, length,
element size, data offset, and a process-shared mutex for thread-safety. The
data payload is stored in a separate allocator allocation.

Current implementation:

- `dynarray_create(...)` allocates a new array with an initial capacity
- `dynarray_push(...)` appends an element, growing capacity if needed
- `dynarray_get(...)` retrieves an element by index
- `dynarray_reserve(...)` explicitly grows capacity
- `dynarray_length(...)` returns the current element count
- `dynarray_destroy(...)` frees the array and its payload

The array uses a simple doubling strategy for growth: when capacity is reached,
a new buffer twice the size is allocated, existing data is copied, and the old
buffer is freed.

### Linked List (intrusive)

`dynlist` provides a doubly-linked list with intrusive nodes. Each node stores
the user payload directly after the node header, avoiding separate allocations
per element. The list header tracks head/tail offsets, length, element size,
and a process-shared mutex.

Current implementation:

- `dynlist_create(...)` allocates a new empty list
- `dynlist_push_back(...)` appends an element at the tail
- `dynlist_push_front(...)` inserts an element at the head
- `dynlist_get(...)` retrieves an element by index (linear traversal)
- `dynlist_length(...)` returns the current element count
- `dynlist_destroy(...)` frees all nodes and the list header

Both containers use robust process-shared mutexes to handle process crashes
gracefully.

Current implemented root-discovery behavior:

- the private region header contains a fixed-capacity root table
- each root binds a short stable name to an `OffsetPtr`
- roots are intended for well-known entry-point objects such as shared config,
  top-level indexes, or coordination objects
- root-table operations take the roots rwlock internally
- freeing a rooted object does not automatically remove the root, so callers
  must keep root bindings in sync with object lifetime

Root-table algorithm:

1. The private region header contains a fixed-capacity array of root entries.
2. Each entry stores an occupancy flag, a short inline name, and an `OffsetPtr`.
3. `offset_store_set_root(...)` takes the roots write lock, scans for an existing matching
   name, and otherwise uses the first free slot.
4. `offset_store_get_root(...)` takes the roots read lock, performs a linear scan for the
   requested name, and returns the stored handle.
5. `offset_store_remove_root(...)` takes the roots write lock, clears the matching entry,
   and resets its handle to null.

This is intentionally not a hash table yet. The fixed array keeps layout and
validation simple while still solving the immediate discovery problem.

Current implemented shared-index behavior:

- the private region header also contains a fixed-capacity index table
- each index entry binds a short key to an `OffsetPtr`
- index operations use linear scans under the index rwlock
- the index is meant for general small directories, while roots remain the
  smaller set of well-known entry points

Index algorithm:

1. The private region header contains a fixed-capacity array of index entries.
2. Each entry stores an occupancy flag, a short inline key, and an `OffsetPtr`.
3. `offset_store_index_put(...)` takes the index write lock, scans for an existing
   matching key, and otherwise uses the first free slot.
4. `offset_store_index_get(...)` takes the index read lock, performs a linear scan for
   the requested key, and returns the stored handle.
5. `offset_store_index_contains(...)` takes the index read lock and reports whether the
   key is present.
6. `offset_store_index_remove(...)` takes the index write lock, clears the matching
   entry, and resets its handle to null.

## Build And Debug Workflow

Current build entry points:

- `make` builds the example binaries into `build/`
- `make examples` builds `build/producer` and `build/consumer`
- `make test` builds and runs the default deterministic unit/integration suite
- `make stress` builds and runs the heavier stress-oriented test binaries
- `make test-asan` builds and runs tests with AddressSanitizer instrumentation
- `make test-ubsan` builds and runs tests with UndefinedBehaviorSanitizer instrumentation
- `make test-sanitize` builds and runs tests with both ASan and UBSan instrumentation
- `make stress-asan`, `make stress-ubsan`, `make stress-sanitize` for sanitizer-backed stress tests
- `make clean` removes `build/`

Continuous integration:

- GitHub Actions now runs a minimal CI workflow on pushes and pull requests
- the workflow builds the examples and runs `make test`
- the heavier `make stress` target remains opt-in and is not part of the default CI path

Example flow:

1. Run `make examples`.
2. Start the producer:
   `./build/producer /offset-store-demo greeting "hello from shared memory"`
3. Note the printed root name and object offset.
4. In another shell, run the consumer:
   `./build/consumer /offset-store-demo greeting`
5. When done, remove the shared memory object manually if needed:
   `rm /dev/shm/offset-store-demo`

Recommended debugging workflow:

- use `make test` as the baseline correctness check
- `make test` and `make stress` now print `Running build/test_...` before each
  binary so slow or stuck suites are easy to identify
- use `make stress` for heavier soak-style concurrency coverage that is kept
  out of the default `make test` path
- the allocator test binary now includes a multi-process churn stress test that
  forks several workers, repeatedly allocates and frees varied payload sizes,
  and then checks allocator validation plus final heap stats in the parent
- the store test binary now includes a roots/index reader-writer contention
  stress test with pre-attached worker processes, concurrent writers, and
  lookup readers to exercise the current directory rwlock model under load
- the dedicated `test_store_stress` binary run by `make stress` includes a
  mixed full-system stress test that
  begins with a synchronized stable-publication phase and then combines object
  allocation/free churn, root/index publication, concurrent readers, and
  periodic allocator-stat snapshots with final quiescent whole-store
  validation, while tolerating expected out-of-memory skips during the churn
  phase
- the lock contention stress binary now runs as a proper Unity suite and uses
  real pthread worker return statuses instead of process-exit shortcuts
- the crash simulation tests (`test_crash_simulation_stress`) verify allocator
  and object-store behavior under simulated crash scenarios including child
  process termination, multi-process churn with forced termination, and
  orphan allocation handling
- the corruption handling tests (`test_corruption_handling`) verify that validation
  and mutation APIs correctly detect, reject, and handle various forms of
  data corruption including corrupted allocator headers, invalid free lists,
  damaged block headers, and invalid offset pointers
- the lock contention tests (`test_lock_contention_stress`) verify that the
  locking model works correctly under multi-threaded load, exercising
  allocator mutex contention, roots rwlock contention, index rwlock contention,
  and mixed-subsystem lock contention scenarios
- attach `gdb` to the example binaries for step-by-step shared-memory inspection
- run the test or example binaries under `valgrind` when investigating memory misuse
- use `make test-sanitize` for memory safety checking during development
- inspect `/dev/shm` to confirm POSIX shared-memory objects are being created and removed

Note on portability:

- the robust allocator-mutex implementation is enabled in the library where the
  platform supports it
- a direct owner-death regression was removed from the default test suite
  because process-shared robust mutex recovery is not portable enough to keep
  `make test` deterministic across environments

Current public error-reporting direction:

- mutating public APIs now return `OffsetStoreStatus`
- `offset_store_status_string(...)` converts those codes into readable text
- pointer-returning accessors such as object/header resolution still use `NULL` on failure
- the remaining API-polish work will extend this contract more consistently across the surface

Current lifecycle direction:

- prefer `OffsetStore` for common create/open/close flows
- `offset_store_bootstrap(...)` performs region creation plus allocator initialization
- `offset_store_open_existing(...)` performs region attach plus allocator validation
- initialization is currently strict one-shot rather than idempotent
- duplicate bootstrap/create attempts fail with `OFFSET_STORE_STATUS_ALREADY_EXISTS`
- duplicate allocator initialization fails with `OFFSET_STORE_STATUS_ALREADY_EXISTS`
- lower-level `shm_region` and `allocator` APIs remain available when finer-grained control is needed

Current accessor naming direction:

- query-style helpers now consistently use `get` prefixes
- examples include `shm_region_get_version(...)`, `allocator_get_heap_size(...)`,
  and `object_store_get_payload(...)`
- mutable object accessors use the explicit `_mut` suffix via
  `object_store_get_header_mut(...)`
- read-only region data access now uses `shm_region_data_const(...)` while
  mutable access continues through `shm_region_data(...)`

Current allocator introspection direction:

- `allocator_get_stats(...)` reports heap size, free bytes, used bytes, largest
  free block, free block count, and cumulative allocation failures
- stats are derived from a read-only heap walk rather than persistent shared
  counters, except for the persistent allocation-failure count stored in
  allocator metadata

## API Usage

Recommended high-level create flow:

1. Call `offset_store_bootstrap(...)` to create, map, and initialize a store.
2. Check the returned `OffsetStoreStatus`.
3. Optionally call `offset_store_validate(...)` after setup when a caller wants an explicit integrity check.
4. Allocate shared objects and bind well-known ones with `offset_store_set_root(...)` when cross-process discovery is needed.
5. Use `offset_store_index_put(...)` when a caller needs a small shared
   key/value directory rather than just a few well-known roots.
6. Call `offset_store_close(...)` when the process is done with the mapping.

Bootstrap algorithm:

1. Create a POSIX shared-memory object with `shm_open(..., O_CREAT | O_EXCL, ...)`.
2. Resize it with `ftruncate(...)`.
3. Map it with `mmap(...)`.
4. Write the private region header, initialize the process-shared allocator
   mutex plus roots/index rwlocks, and clear the fixed root and index tables.
5. Initialize allocator metadata and seed the heap with one large free block.
6. Return a process-local `OffsetStore` wrapper that points at the mapping.

Recommended high-level attach flow:

1. Call `offset_store_open_existing(...)` to attach to an existing store.
2. Check the returned `OffsetStoreStatus`.
3. Optionally call `offset_store_validate(...)` to verify the attached region and allocator state explicitly.
4. Resolve well-known objects with `offset_store_get_root(...)` and then use their `OffsetPtr` handles.
5. Resolve indexed objects with `offset_store_index_get(...)` when they are
   discovered through the shared directory rather than by root name.
6. Call `offset_store_close(...)` when finished.

Attach algorithm:

1. Open the existing shared-memory object with `shm_open(...)`.
2. Discover its size with `fstat(...)`.
3. Map it with `mmap(...)`.
4. Validate the region header magic, layout version, and recorded total size.
5. Validate allocator metadata before treating the store as usable.
6. Resolve roots or object handles only after the region and allocator checks pass.

Recommended low-level create flow:

1. Call `shm_region_create(...)`.
2. Call `allocator_init(...)` exactly once for the new region.
3. Use `object_store_alloc(...)` or `allocator_alloc(...)` for shared storage.
4. Call `shm_region_close(...)` when done.
5. Call `shm_region_unlink(...)` when the shared-memory object should be removed.

Recommended object access flow:

1. Keep object references as `OffsetPtr` values.
2. Persist or exchange stable discovery names through the root table when other processes need to find top-level objects.
3. Resolve headers with `object_store_get_header(...)` or `object_store_get_header_mut(...)`.
4. Resolve payloads with `object_store_get_payload_const(...)` or `object_store_get_payload(...)`.
5. Use `object_store_validate(...)` when a caller needs an explicit status-based integrity check for one object handle.
6. Treat returned raw pointers as process-local and transient.
7. Remove any root bindings before freeing rooted objects with `object_store_free(...)`.
8. Do not reuse object handles after free; freed objects are explicitly marked and rejected by accessors.

Recommended error-handling pattern:

- check every `OffsetStoreStatus` return before continuing
- convert failures to readable text with `offset_store_status_string(...)`
- treat `OFFSET_STORE_STATUS_ALREADY_EXISTS` as a normal duplicate-create case
- treat `OFFSET_STORE_STATUS_NOT_FOUND` as a normal missing-object or missing-region case
- treat `NULL` from pointer-returning accessors as resolution failure and avoid dereferencing
- close any successfully opened region/store descriptor on the error path before returning

Current validation surface:

- `shm_region_validate(...)` checks the private shared-region header
- `allocator_validate(...)` checks allocator metadata plus heap/free-list structure
- `object_store_validate(...)` checks one specific object handle
- `offset_store_validate(...)` checks the region header and allocator state together
- there is not yet a heap-wide object scan because the current layout does not
  maintain a global object registry

Recommended synchronization pattern:

- rely on allocator/object allocation and free paths for internal allocator locking
- use the dedicated root and index APIs for publication work that should not contend with allocator mutation
- take the relevant subsystem lock explicitly when a caller needs a stable multi-step sequence
- avoid keeping resolved raw pointers across calls that may free or remap the underlying object
- prefer re-resolving from `OffsetPtr` after synchronization is re-established

Potential future header fields:

- flags
- checksum or canary
- generation or epoch markers

Any header expansion needs careful ABI/layout review because all processes must agree
on structure sizes and alignment.

## Synchronization Model

Multiple processes may allocate, free, or mutate objects concurrently. Shared state
therefore requires explicit synchronization that is safe across process boundaries.


Synchronization design rules:

- synchronization primitives protecting shared state must be compatible with
  multi-process usage
- lock ownership assumptions must account for process crashes
- shared metadata updates should happen in an order that leaves structures
  inspectable during debugging

Current implemented synchronization behavior:

- one process-shared `pthread_mutex_t` plus two process-shared
  `pthread_rwlock_t` values live in `ShmRegionHeader`
- `shm_region_create` initializes the allocator mutex and the roots/index
  rwlocks for multi-process use
- `shm_region_validate(...)` now checks both header fields and that each
  subsystem lock is operational
- allocator mutation paths (`allocator_init`, `allocator_alloc`, `allocator_free`)
  take only the allocator mutex
- root-table paths take only the roots rwlock, using read locks for lookup and
  write locks for put/remove
- shared-index paths take only the index rwlock, using read locks for lookup
  and contains plus write locks for put/remove
- there is no multi-lock public operation yet, so current operations do not
  require cross-subsystem lock ordering

**Evaluation of read-write synchronization and atomic fast paths:**

The current design uses a separate rwlock for the roots table and another for the
index table, providing natural read-write semantics:

- Roots lookups use read locks, allowing concurrent readers when no writer is present
- Index lookups similarly benefit from concurrent readers
- Write operations (set_root, remove_root, index_put, index_remove) acquire write
  locks, serializing with readers and other writers

This design was chosen because:

1. Read-heavy workloads benefit from rwlock semantics - multiple processes can
   simultaneously query roots or indexes without blocking each other
2. The fixed-capacity tables are small (typically < 100 entries), so linear scan
   under read lock is acceptable
3. The rwlock is POSIX-standard and process-shared compatible

Alternative designs considered and reasons for deferral:

- **Lock-free hash tables**: Attractive for high-contention scenarios but add
  significant complexity; the current fixed-array design keeps layout simple
- **Atomic operations on individual entries**: Would require careful atomic
  CAS semantics on the shared offset pointers; currently safe because rwlock
  provides atomic visibility
- **Seperate read/write paths in allocator**: The allocator mutation path is
  already fast-path optimized; adding read locks would not improve throughput
  since allocation/free are inherently write operations

Future considerations:

- If roots/index tables grow beyond hundreds of entries, switching to a lock-free
  hash table could improve lookup scalability
- Atomic fast paths have been added for simple increment operations: the
  `allocation_failures` counter in `AllocatorHeader` uses `_Atomic`
  with `memory_order_relaxed` to allow lock-free reads without requiring
  the full allocator mutex
- Sharded locking has been implemented for the allocator: the heap is
  partitioned into N shards (currently 4), each with its own process-shared
  mutex and free list. This reduces contention under high-load multi-process
  scenarios by allowing concurrent allocations from different shards.
  Shard selection uses size-based routing to improve locality.

Canonical lock order for any future multi-lock path:

- allocator (any shard mutex)
- roots
- index

Current internal-locking contract:

- acquires one of the shard mutexes (sharded locking):
  `allocator_init(...)`, `allocator_alloc(...)`, `allocator_free(...)`
- acquires the roots rwlock internally:
  `shm_region_set_root(...)`, `shm_region_get_root(...)`,
  `shm_region_remove_root(...)`, `offset_store_set_root(...)`,
  `offset_store_get_root(...)`, `offset_store_remove_root(...)`
- acquires the index rwlock internally:
  `shm_region_index_put(...)`, `shm_region_index_get(...)`,
  `shm_region_index_contains(...)`, `shm_region_index_remove(...)`,
  `offset_store_index_put(...)`, `offset_store_index_get(...)`,
  `offset_store_index_contains(...)`, `offset_store_index_remove(...)`
- does not acquire subsystem locks internally:
  `allocator_validate(...)`, allocator metadata query helpers,
  `object_store_get_header(...)`, `object_store_get_header_mut(...)`,
  `object_store_get_payload_const(...)`, `object_store_get_payload(...)`,
  and the `shm_region` metadata/query helpers other than explicit
  subsystem lock helpers such as `shm_region_allocator_lock(...)`,
  `shm_region_roots_lock(...)`, and `shm_region_index_lock(...)`
- callers that need a stable read view during concurrent mutation must provide
  external synchronization themselves, typically by taking the relevant
  subsystem lock
- higher-level store flows such as bootstrap/open/validate currently do not
  nest subsystem locks; they remain single-subsystem operations today

Current consistency model for reads during concurrent mutation:

- object allocation and object free are serialized because they flow through the
  allocator mutation path
- object/header/payload accessors validate bounds and allocator ownership, but
  they do not provide snapshot isolation
- concurrent payload mutation by one process may be observed directly by another
  process performing unsynchronized reads
- concurrent object free or allocator mutation can invalidate a previously
  resolved process-local pointer unless the caller holds external synchronization

## Required Invariants

These are hard constraints for the project:

1. No absolute pointers stored in shared memory.
2. All references in shared-memory objects use offset-based addressing.
3. Allocator metadata lives inside the shared region.
4. All processes agree on structure layout.
5. Objects do not move after allocation unless compaction is explicitly implemented.

If any implementation choice conflicts with one of these invariants, the implementation
choice is wrong.

## Failure Model

The initial system assumes cooperative processes, but it should still be designed with
failure awareness in mind.

Potential failure cases:

- process crash while holding a lock
- partial writes to metadata
- allocator corruption
- invalid offsets
- double-free or stale-reference bugs
- disagreement about structure layout between binaries

Early-stage expectations:

- correctness checks are more important than high throughput
- validation hooks should exist where practical
- the code should fail loudly in debug-oriented builds

Possible future resilience features:

- allocator journaling
- recovery scans on startup
- consistency verification passes
- canaries and checksums
- generation-based validation

## Debugging and Validation

The project should remain friendly to low-level debugging tools.

Recommended tools:

- `valgrind`
- AddressSanitizer
- `gdb`
- `perf`
- `strace`

Helpful debug features for future implementation:

- allocator canaries
- heap validation routines
- offset bounds checks
- region header sanity checks
- build-time assertions for structure sizes and offsets

Examples of checks worth adding:

- verify that an offset resolves inside the mapped region
- verify that free-list links point to valid blocks
- verify that object header sizes are aligned and in-range
- verify that synchronization objects were initialized correctly


## Module Responsibilities

The intended responsibilities of the planned modules are:

### `shm_region`

Current responsibilities:

- create or attach to the shared memory object
- size and map the shared region
- expose region metadata such as base address and total size
- coordinate initialization of region-level headers
- provide the process-shared allocator mutex plus the roots/index rwlocks

Key constraint:

- any persistent metadata written by this module must be address-independent

### `offset_ptr`

Current responsibilities:

- define the shared offset pointer type
- convert between offsets and transient local pointers
- validate bounds where appropriate
- centralize offset arithmetic so the rest of the codebase does not duplicate it

Key constraint:

- conversion helpers must never normalize the codebase into storing raw shared pointers

### `allocator`


Current responsibilities:

- initialize allocator metadata in the shared region
- allocate and free blocks from the shared heap
- maintain free-list state
- provide validation hooks for debugging

Key constraint:

- allocator bookkeeping must be entirely contained in shared memory

### `object_store`


Current responsibilities:

- provide object-level APIs on top of the allocator
- manage object headers and type metadata
- encode references using offsets
- eventually support lookup or indexing abstractions if introduced

Key constraint:

- object APIs should preserve stable layout and not hide process-local pointer storage

### `examples`

Expected responsibilities:

- demonstrate producer/consumer or multi-process interactions
- show correct attach, allocate, write, and read flows
- serve as behavioral documentation for users of the project

### `tests`

Expected responsibilities:

- verify allocator correctness
- validate offset conversions
- exercise multi-process attachment behavior where practical
- catch layout regressions and invariants violations

## Development Rules

Project-specific workflow currently requires:

1. Before making any code or documentation change, identify which files will change.
2. Explain how the change affects other files or modules.
3. Describe downstream changes that will be required.
4. Ask for approval before editing anything.
5. After every change, update the relevant README documentation before considering the
   work complete.
6. Update `TODO.md` as work progresses so completed and remaining tasks stay accurate.

These workflow requirements are recorded in `AGENTS.md` and should be followed for all
future modifications.

## Design Constraints for Contributors

When adding or changing code:

- never store raw pointers in shared-memory-resident structures
- keep structure layout explicit and deterministic
- assume multiple processes may observe the same bytes concurrently
- avoid hidden allocations outside the shared allocator for shared-state features
- document memory layout, synchronization strategy, and failure behavior for new
  shared data structures

Useful implementation habits:

- use fixed-width integer types for on-region metadata
- consider alignment and padding explicitly
- keep serialization implicit by relying on stable binary layout only when that layout
  is well-defined and controlled
- add assertions for offsets, sizes, and header invariants early

## Example Conceptual Flow

The expected usage model for a simple producer/consumer system is:

1. Process A creates the shared memory region.
2. Process A initializes region metadata and allocator state.
3. Process A allocates an object and writes its payload.
4. Process A stores references to other objects using offsets.
5. Process B attaches to the same shared memory object.
6. Process B resolves offsets relative to its own mapped base address.
7. Process B reads or updates shared objects using the synchronization protocol.

This flow is possible only because object references are not stored as absolute
addresses.

## Non-Goals for the Initial Version

The initial project description does not assume:

- persistent durability across reboot
- automatic crash recovery
- lock-free performance tuning
- moving garbage collection
- transparent pointer-like ergonomics at the cost of layout clarity

These may become future extensions, but they should not complicate the first working
implementation.

## Long-Term Directions

Potential future work includes:

- persistent shared memory backing
- crash recovery and allocator repair
- lock-free or low-contention allocation strategies
- shared indexing structures
- multi-process hash tables
- NUMA-aware placement

Each of these would require revisiting:

- metadata layout
- synchronization strategy
- recovery semantics
- testing strategy
