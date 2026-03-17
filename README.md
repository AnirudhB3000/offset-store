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
implementation helpers, tests, and examples.

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
- initialize and expose a process-shared mutex stored in the region header
- store a fixed-capacity named root table for well-known shared objects
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
- allocation supports caller-specified power-of-two alignment
- freeing reconstructs the owning block from an in-band prefix that stores the
  block offset rather than a raw pointer
- `allocator_validate` walks the physical heap and the free list to catch basic
  structural corruption
- allocator metadata layout remains internal to `src/allocator.c`; the public API
  now exposes small query helpers instead of the raw allocator header struct
- `allocator_get_stats(...)` provides a snapshot of heap usage and free-space
  fragmentation by walking the current block layout at call time

Allocator algorithm:

1. `allocator_init(...)` computes where the allocator header ends, rounds the
   heap start up to `max_align_t`, and creates one initial free block covering
   the remaining bytes.
2. `allocator_alloc(...)` walks the free list in first-fit order.
3. For each candidate block, it computes where a payload could start after the
   block header and allocation prefix, then rounds that payload start up to the
   caller's requested alignment.
4. If the block is large enough, the allocator either consumes the whole block
   or splits it into an allocated block plus a remainder free block.
5. Immediately before the returned payload, the allocator writes a small prefix
   containing the owning block offset.
6. `allocator_free(...)` reads that prefix back, reconstructs the block, checks
   that it still looks like a live allocation, and pushes it onto the free list.

This is intentionally a simple first-fit allocator. It does not currently
coalesce adjacent free blocks, so fragmentation is possible after mixed-size
allocation patterns.

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

Current implemented root-discovery behavior:

- the private region header contains a fixed-capacity root table
- each root binds a short stable name to an `OffsetPtr`
- roots are intended for well-known entry-point objects such as shared config,
  top-level indexes, or coordination objects
- root-table operations take the region mutex internally
- freeing a rooted object does not automatically remove the root, so callers
  must keep root bindings in sync with object lifetime

Root-table algorithm:

1. The private region header contains a fixed-capacity array of root entries.
2. Each entry stores an occupancy flag, a short inline name, and an `OffsetPtr`.
3. `offset_store_set_root(...)` locks the region, scans for an existing matching
   name, and otherwise uses the first free slot.
4. `offset_store_get_root(...)` locks the region, performs a linear scan for the
   requested name, and returns the stored handle.
5. `offset_store_remove_root(...)` locks the region, clears the matching entry,
   and resets its handle to null.

This is intentionally not a hash table yet. The fixed array keeps layout and
validation simple while still solving the immediate discovery problem.

## Build And Debug Workflow

Current build entry points:

- `make` builds the example binaries into `build/`
- `make examples` builds `build/producer` and `build/consumer`
- `make test` builds and runs the unit test suite
- `make clean` removes `build/`

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
- attach `gdb` to the example binaries for step-by-step shared-memory inspection
- run the test or example binaries under `valgrind` when investigating memory misuse
- inspect `/dev/shm` to confirm POSIX shared-memory objects are being created and removed

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
  free block, and free block count
- stats are derived from a read-only heap walk rather than persistent shared
  counters
- allocation-failure counters are not yet tracked in shared metadata

## API Usage

Recommended high-level create flow:

1. Call `offset_store_bootstrap(...)` to create, map, and initialize a store.
2. Check the returned `OffsetStoreStatus`.
3. Optionally call `offset_store_validate(...)` after setup when a caller wants an explicit integrity check.
4. Allocate shared objects and bind well-known ones with `offset_store_set_root(...)` when cross-process discovery is needed.
5. Call `offset_store_close(...)` when the process is done with the mapping.

Bootstrap algorithm:

1. Create a POSIX shared-memory object with `shm_open(..., O_CREAT | O_EXCL, ...)`.
2. Resize it with `ftruncate(...)`.
3. Map it with `mmap(...)`.
4. Write the private region header, initialize the process-shared mutex, and
   clear the fixed root table.
5. Initialize allocator metadata and seed the heap with one large free block.
6. Return a process-local `OffsetStore` wrapper that points at the mapping.

Recommended high-level attach flow:

1. Call `offset_store_open_existing(...)` to attach to an existing store.
2. Check the returned `OffsetStoreStatus`.
3. Optionally call `offset_store_validate(...)` to verify the attached region and allocator state explicitly.
4. Resolve well-known objects with `offset_store_get_root(...)` and then use their `OffsetPtr` handles.
5. Call `offset_store_close(...)` when finished.

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

- rely on allocator/object allocation and free paths for internal mutation locking
- take the region mutex explicitly when a caller needs a stable multi-step read or write sequence
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

Initial synchronization mechanism:

- `pthread_mutex` configured as process-shared

Likely scope for the initial lock:

- allocator mutation
- shared metadata updates
- object index or registry mutation if added later

Possible future refinements:

- finer-grained locking
- sharded allocators
- read/write locking
- lock-free data structures
- targeted atomic protocols

Synchronization design rules:

- synchronization primitives protecting shared state must be compatible with
  multi-process usage
- lock ownership assumptions must account for process crashes
- shared metadata updates should happen in an order that leaves structures
  inspectable during debugging

Current implemented synchronization behavior:

- a process-shared `pthread_mutex_t` lives in `ShmRegionHeader`
- `shm_region_create` initializes the mutex for multi-process use
- allocator mutation paths (`allocator_init`, `allocator_alloc`, `allocator_free`)
  take the region mutex before touching shared allocator metadata
- the current lock scope is coarse-grained and protects allocator/shared-region
  mutation rather than individual structures

Current internal-locking contract:

- acquires the region mutex internally:
  `allocator_init(...)`, `allocator_alloc(...)`, `allocator_free(...)`
- does not acquire the region mutex internally:
  `allocator_validate(...)`, allocator metadata query helpers,
  `object_store_get_header(...)`, `object_store_get_header_mut(...)`,
  `object_store_get_payload_const(...)`, `object_store_get_payload(...)`,
  and the `shm_region` metadata/query helpers other than explicit
  `shm_region_lock(...)` and `shm_region_unlock(...)`
- callers that need a stable read view during concurrent mutation must provide
  external synchronization themselves, typically by taking the region mutex

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

## Planned Repository Structure

The design documents the following intended structure:

```text
/src
    shm_region.c
    allocator.c
    offset_ptr.c
    object_store.c

/include
    shm_region.h
    allocator.h
    offset_ptr.h

/examples
    producer.c
    consumer.c

/tests
    allocator_tests.c
```

Current repository status:

- `AGENTS.md` exists and defines project constraints and workflow instructions
- `include/`, `src/`, `tests/`, `examples/`, `docs/`, and `build/` now exist as the
  initial scaffold
- `offset_ptr`, `shm_region`, `allocator`, and `object_store` have been implemented
  with tests
- the remaining implementation files listed above are not yet present

As code is added, this README should be updated to reflect the actual structure rather
than only the intended one.

## Module Responsibilities

The intended responsibilities of the planned modules are:

### `shm_region`

Current status:

- implemented

Current responsibilities:

- create or attach to the shared memory object
- size and map the shared region
- expose region metadata such as base address and total size
- coordinate initialization of region-level headers
- provide the process-shared region mutex used by allocator mutation

Key constraint:

- any persistent metadata written by this module must be address-independent

### `offset_ptr`

Current status:

- implemented

Current responsibilities:

- define the shared offset pointer type
- convert between offsets and transient local pointers
- validate bounds where appropriate
- centralize offset arithmetic so the rest of the codebase does not duplicate it

Key constraint:

- conversion helpers must never normalize the codebase into storing raw shared pointers

### `allocator`

Current status:

- implemented

Current responsibilities:

- initialize allocator metadata in the shared region
- allocate and free blocks from the shared heap
- maintain free-list state
- provide validation hooks for debugging

Key constraint:

- allocator bookkeeping must be entirely contained in shared memory

### `object_store`

Current status:

- implemented

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

## Current State

The repository is currently in a design-definition stage.
The `offset_ptr`, `shm_region`, `allocator`, and `object_store` modules are
implemented; the rest of the system remains in the design and scaffolding stage.

Present top-level files and directories:

- `AGENTS.md`
- `LICENSE.txt`
- `Makefile`
- `README.md`
- `TODO.md`
- `build/`
- `docs/`
- `examples/`
- `include/`
- `src/`
- `tests/`

Implemented files:

- `include/offset_store/offset_store.h`
- `include/offset_store/offset_ptr.h`
- `include/offset_store/allocator.h`
- `include/offset_store/object_store.h`
- `include/offset_store/shm_region.h`
- `include/offset_store/store.h`
- `src/allocator.c`
- `src/offset_store.c`
- `src/object_store.c`
- `src/offset_ptr.c`
- `src/shm_region.c`
- `src/store.c`
- `tests/test_allocator.c`
- `tests/test_offset_store.c`
- `tests/test_object_store.c`
- `tests/test_offset_ptr.c`
- `tests/test_shm_region.c`
- `tests/test_store.c`
- `docs/memory-layout.md`
- `examples/consumer.c`
- `examples/producer.c`

Absent today but expected later:

- most implementation sources
- most headers
- additional tests
- example programs

That means this README is intentionally architecture-heavy. As implementation lands, it
should evolve from a design document into a combined design and usage document grounded
in the actual codebase.

## Current Testing

The repository now includes a first unit test:

- `tests/test_offset_ptr.c`
- `tests/test_allocator.c`
- `tests/test_offset_store.c`
- `tests/test_object_store.c`
- `tests/test_shm_region.c`
- `tests/test_store.c`

The current `Makefile` builds all test files under `tests/` and links them against the
current sources under `src/`, together with the vendored Unity framework under
`third_party/unity/`. Running `make test` executes every produced test binary.
Each test remains a standalone executable, but assertion reporting now comes from
Unity rather than raw C `assert(...)`. After the per-binary Unity output, `make test`
also prints a suite-level summary of how many tests ran, passed, failed, and were
ignored.
The shared-memory test suite includes a fork-based check that verifies the region mutex
coordinates access across processes.
The same `Makefile` also builds the example binaries under `build/`.

## Contribution Expectation

Changes should be incremental, explicit, and easy to review.

A good change in this repository should:

- preserve the core invariants
- explain its impact on layout and concurrency
- avoid hidden process-local assumptions
- update documentation when behavior or structure changes

For any new shared structure, document at minimum:

- memory layout
- synchronization strategy
- failure behavior
