# Veloco Bounded Contexts

Veloco is split into four bounded contexts: Runtime, Memory, Async I/O,
and HTTP. Each context owns its lifecycle and exposes a narrow public
API through `include/veloco/*.h`. The `ef/` tree is an external
black-box reference and belongs to none of these contexts.

## Context map

| Context | Owns | Consumes | Provides |
| --- | --- | --- | --- |
| Runtime | Fiber, Task/G, P, M, scheduler, run queues, timers, synchronization primitives, shutdown | Task functions, I/O completions, allocator statistics | `include/veloco/runtime.h`, `include/veloco/task.h`, `include/veloco/sync.h`, `include/veloco/timer.h` |
| Memory | SizeClass, Span, PageHeap, P-local caches, central free list, Arena, object pools, debug allocator | Linux `mmap`/`munmap`, scheduler safe points for remote frees | `include/veloco/memory.h` |
| Async I/O | I/O Requests, Backend interface, io_uring/epoll rings, SQE/CQE, Generation tokens | FDs, Task parking decisions, runtime workers | `include/veloco/io.h` |
| HTTP | Connection, Parser, Router, Response, middleware, connection limits, backpressure | Public Task, allocator, and async I/O APIs | `include/veloco/http.h` |

## Ownership rules

- Runtime owns Task scheduling and every state transition shown in
  [docs/diagrams/task-lifecycle.md](../diagrams/task-lifecycle.md).
- In Task 4, Runtime is a single-thread context: one `vl_runtime_t` owns one
  Fiber scheduler, one intrusive FIFO runnable queue, and all Task handles.
  `vl_spawn` appends RUNNABLE Tasks, `vl_yield` re-appends the current Task,
  and `vl_join` parks it on the target wait list.
- Memory owns the allocation lifecycle from `vl_malloc`/`vl_free`
  through spans and the page heap to `mmap`/`munmap`.
- Async I/O owns kernel operation completion: submit an SQE, reap the
  CQE, validate the generation, store the result, and wake the Task.
- HTTP owns protocol behavior and per-Connection/request lifetime; it
  uses only the public Runtime, Memory, and Async I/O APIs.
- Public headers never expose epoll or io_uring-specific types.

## Shared language and anti-corruption notes

- Task/G crosses all contexts: Runtime schedules it, Async I/O wakes it,
  HTTP runs it. Ownership of a Task never leaves Runtime.
- The P-local allocator cache crosses Runtime and Memory: Runtime owns
  the P's identity and safe points, Memory owns the cache contents.
- Arena lifetime is driven by HTTP (per request) but implemented by
  Memory; HTTP must call `vl_arena_reset` at request teardown and never
  hand arena pointers past that point.
- The Async I/O Backend interface exposes operations and results only;
  epoll readiness-style types must not leak through the interface, so
  io_uring and epoll remain swappable.
- `ef/` is a black-box reference only: behavior and benchmark numbers
  may be recorded, but no `ef/` source, headers, assembly, utility code,
  or tests are copied into Veloco. Veloco assertions derive from public
  behavior and independently stated invariants, not from `ef/` tests.

## Explicitly deferred boundaries

Dynamic M creation and preemptive scheduling are deferred beyond the
fixed-worker first release. HTTP/2, TLS, a tracing GC, NUMA-aware
allocation, distributed clustering, and general asynchronous disk I/O
are out of scope for the first release. P-sharded rings and io_uring
optimization experiments are gated behind the Task 6 correctness and
benchmark baseline.
