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

Possible upgrades:

* lock-free structures
* atomic operations
* sharded allocators

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

# Repository Structure

```
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

Initial version assumes cooperative processes.

Later versions may implement:

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
10. Update `TODO.md` as work progresses so completed and remaining tasks stay accurate

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
