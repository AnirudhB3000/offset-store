# AGENTS.md

## Project Overview

This repository implements a **Shared Memory Object Store**.

Multiple processes attach to the same shared memory region and interact with a common object heap. The system provides:

* a shared allocator
* offset-based pointer addressing
* concurrent access from multiple processes
* crash-resilient object metadata

The core challenge is that **virtual addresses differ across processes**, so raw pointers cannot be used.

Instead, objects reference each other using **offset pointers relative to the shared memory base**.

---

# Core Concepts

## Shared Memory Region

All data lives inside a single shared memory segment.

Example:

```
+-------------------------+
| heap metadata           |
+-------------------------+
| allocator structures    |
+-------------------------+
| object storage          |
+-------------------------+
| free blocks             |
+-------------------------+
```

Processes attach via:

```
shm_open
mmap
```

The shared memory base address may differ per process.

Therefore:

```
absolute pointers are forbidden
```

Only **relative offsets** may be stored.

---

# Pointer Model

## Offset Pointer

Objects reference other objects using offsets.

Example:

```
struct OffsetPtr {
    uint64_t offset;
}
```

To resolve:

```
resolved_ptr = base_address + offset
```

To store:

```
offset = ptr - base_address
```

Rules:

* offsets are always relative to the shared memory base
* zero offset means NULL

---

# Allocator Design

The shared heap uses a custom allocator.

Goals:

* deterministic layout
* minimal fragmentation
* safe multi-process access

Initial implementation:

```
free list allocator
```

Later possible upgrades:

* slab allocator
* segregated size classes
* buddy allocator

Allocator metadata must also reside inside shared memory.

---

# Synchronization

Multiple processes may allocate or modify objects concurrently.

The system must provide synchronization.

Initial mechanism:

```
pthread_mutex (process-shared)
```

Current implementation features:

* Sharded allocator locks: heap partitioned into N shards (currently 4), each with
  its own process-shared mutex and free list, reducing contention under high-load
* Robust mutex attributes (PTHREAD_MUTEX_ROBUST) for crash resilience on all internal
  mutexes (allocator shards, dynarray, dynlist, hashtable, ringbuf)
* Lock recovery handling for EOWNERDEAD returns after process crash
* Atomic operations for simple counters (allocation_failures) using _Atomic with
  memory_order_relaxed

Possible upgrades:

* lock-free structures
* atomic operations
* finer-grained per-object locking

---

# Object Layout

Every object stored in the heap should contain a header.

Example:

```
struct ObjectHeader {
    uint32_t size;
    uint32_t type;
}
```

Objects follow immediately after the header.

Example memory layout:

```
[Header][Object Data]
```

---

# Shared-Memory Containers

The library provides offset-based container types for storing collections in shared memory.

## Dynamic Array (vector)

`dynarray` provides a resizable array stored entirely in shared memory. The header contains capacity, length, element size, data offset, and a process-shared mutex.

Features:
- Doubling growth strategy for amortized O(1) append
- Robust process-shared mutex for crash resilience
- Data stored in separate allocator allocation

## Linked List (intrusive)

`dynlist` provides a doubly-linked list with intrusive nodes. Each node stores the user payload directly after the node header.

Features:
- Head and tail pointers for O(1) append at either end
- Node payload stored inline after node header
- Robust process-shared mutex for crash resilience

---

# Repository Structure

```
/src
    /core
        allocator.c      # Sharded free-list allocator
        offset_ptr.c    # Offset conversion helpers
        offset_store.c  # Status code strings
        shm_region.c    # Shared memory region management
    /store
        object_store.c  # Object allocation layer
        store.c         # High-level store API
    /containers
        dynarray.c      # Dynamic array (vector)
        dynlist.c       # Intrusive linked list
        hashtable.c     # Hash table with chaining
        ringbuf.c       # Ring buffer (bounded queue)

/include/offset_store
    allocator.h
    offset_ptr.h
    offset_store.h
    shm_region.h
    object_store.h
    store.h
    dynarray.h
    dynlist.h
    hashtable.h
    ringbuf.h

/tests
    /core
        test_allocator.c
        test_offset_ptr.c
        test_offset_store.c
        test_shm_region.c
        test_corruption_handling.c
    /store
        test_object_store.c
        test_store.c
    /containers
        test_dynarray.c
    /stress
        test_crash_simulation_stress.c
        test_lock_contention_stress.c
        test_store_stress.c

/examples
    producer.c          # Creates all container types
    consumer.c         # Reads all container types
```

---

# Invariants

These rules must never be violated.

1. No absolute pointers stored in shared memory
2. All references must use offset pointers
3. Allocator metadata must live inside the shared region
4. All processes must agree on structure layout
5. Objects cannot move after allocation (unless compaction is implemented)

---

# Failure Model

Possible failures include:

* process crash
* partial writes
* memory corruption
* inconsistent allocator state

The initial implementation assumes cooperative processes.

Later work may implement:

* allocator journaling
* recovery scans
* consistency checks

---

# Debugging Tools

Recommended tools:

```
valgrind
asan
gdb
perf
strace
```

Debug builds should include:

* allocator canaries
* heap validation
* offset pointer bounds checks

---

# Agent Guidelines

When modifying the code:

1. Never introduce raw pointers into shared memory structures
2. Always use offset-based addressing
3. Ensure new data structures are process-safe
4. Preserve deterministic layout
5. Avoid hidden heap allocations outside the shared allocator
6. Before making any change, identify which files will be modified, explain how the change affects other files or modules, describe any downstream changes that will be required, and ask for approval before editing anything
7. After every change made, update the relevant README documentation before considering the work complete
8. All implemented logic must have corresponding tests
9. All code files must have sufficient inline comments explaining logic
10. All code blocks, including functions, structs, enums, typedefs, and macros, must have sufficient inline documentation using Doxygen-style comments
11. Update `TODO.md` as work progresses so completed and remaining tasks stay accurate
12. Do not add future tasks, roadmap items, or scope changes unless explicitly requested by the user
13. Any user updates made without explicit instruction to modify or remove them must be respected and not reversed
14. If the user removes content from any file, treat that removal as intentional and do not restore or recreate the removed content unless explicitly asked

If new structures are introduced, document:

* memory layout
* synchronization strategy
* failure behavior

---

# Long-Term Goals

Potential future features:

* persistent shared memory store
* lock-free allocator
* object indexing
* multi-process hash table
* crash recovery
* NUMA awareness

---

# Summary

This project explores low-level systems concepts including:

* shared memory
* custom allocators
* pointer relocation
* multi-process coordination
* memory layout design

The repository should remain **simple, deterministic, and heavily documented** to support learning and experimentation.
