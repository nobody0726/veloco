# Allocator Ownership and Flow

The allocator is named `valloc` internally. The fast path is the
P-local cache; misses refill from the central free list, which is
backed by spans from the page heap. The page heap acquires and releases
page ranges through `mmap`/`munmap`.

```mermaid
flowchart LR
    API["vl_malloc / vl_free"] --> LOCAL["owner-thread cache (P-local later)"]
    LOCAL --> CENTRAL["Central free list"]
    CENTRAL --> SPAN["Span (one size class)"]
    SPAN --> PAGES["Page heap"]
    PAGES --> MMAP["mmap / munmap"]
    API --> LARGE["large allocation"]
    LARGE --> PAGES
    ARENA["vl_arena_alloc"] --> ARENA_BLOCK["arena blocks"]
    ARENA_BLOCK --> PAGES
    POOL["vl_pool_alloc"] --> API
```

The cache will belong to P, not M. Task 3 has one thread-affine cache; the
multi-P remote-free path is a later runtime milestone. That future flow is:

```mermaid
flowchart LR
    FREE["vl_free on foreign P"] --> RQ["Owning P remote-free queue"]
    RQ --> SAFE["Drained at scheduler safe point or cache refill"]
    SAFE --> LOCAL["Owning P local cache"]
    LOCAL --> CENTRAL["Central free list (batch drain)"]
```

In Task 3, `cross_p_frees` remains zero because no P exists yet. The diagram
is a domain boundary, not a claim that the multi-thread queue is implemented.

Ownership and sizing rules:

- Small objects are rounded up to a fixed SizeClass table entry; the
  table covers common runtime and HTTP objects without optimizing every
  possible size.
- A span contains objects of exactly one size class and stores its
  metadata outside the user object area so `vl_free` needs no global
  size lookup.
- Refills and drains move between P-local caches and the central free
  list in batches to amortize locking.
- Large allocations bypass the size-class path and come directly from
  the page heap.
- Fiber stacks are allocated separately through guarded `mmap` and are
  not serviced by the normal allocator.
- Arenas (`vl_arena_alloc`) are request-lifetime regions reset by HTTP
  at request teardown; object pools (`vl_pool_alloc`) reuse fixed-size
  objects without changing size.
- Debug builds add canaries, poison patterns, metadata, and double-free
  detection. Allocation is explicit-free only; there is no GC.

Implementation references: `include/veloco/memory.h`,
`src/memory/size_class.c`, `src/memory/span.c`, `src/memory/page_heap.c`,
`src/memory/valloc.c`, `src/memory/arena.c`, `src/memory/object_pool.c`, and
`src/memory/debug_allocator.c` (Task 3).
