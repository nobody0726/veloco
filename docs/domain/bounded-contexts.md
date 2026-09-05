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
| HTTP | Connection, Parser, Router, Response, connection limits, fixed-length and chunked response writing, cooperative shutdown | Public Task, allocator, and async I/O APIs | `include/veloco/http.h` |

## Ownership rules

- Runtime owns Task scheduling and every state transition shown in
  [docs/diagrams/task-lifecycle.md](../diagrams/task-lifecycle.md).
- Runtime synchronization objects are bound to one Runtime. Their FIFO
  waiter lists are protected by the Runtime mutex, and contended operations
  park Tasks so an M remains available.
- Task 7 gives one `vl_runtime_t` a fixed set of P/M pairs, a protected global
  FIFO, P-local Chase-Lev deques, eventfd wake paths, and all Task handles.
  New Tasks can be stolen before first execution; a live Fiber remains bound
  to its first P. `vl_join` parks without blocking the owning M.
- Memory owns the allocation lifecycle from `vl_malloc`/`vl_free`
  through spans and the page heap to `mmap`/`munmap`.
- Async I/O owns kernel operation completion: submit an SQE, reap the
  CQE, validate the generation, store the result, and wake the Task.
- Task 5 establishes that ownership with epoll: the backend borrows a Request
  and buffer until one Completion is consumed, validates tracked fd
  generations, and converts readiness into an accept/recv/send/connect
  result. A Task-bound request parks in WAITING; Runtime drives epoll at a
  scheduler boundary and enqueues the Task as RUNNABLE after completion.
- Task 6 adds one dedicated Ring Worker per io_uring handle. The public owner
  validates Requests and consumes Completions; the worker alone owns liburing
  calls and exchanges commands/completions through synchronized queues.
  Async I/O owns operation and cancellation CQEs, while Runtime retains
  exclusive ownership of Task state transitions.
- HTTP owns protocol behavior and per-Connection/request lifetime; it
  uses only the public Runtime, Memory, and Async I/O APIs. The
  connection task owns the accepted socket, parser buffer, and response
  writer state until the task exits.
- HTTP server shutdown is cooperative: the application stops accepting,
  marks the server shut down, and lets active connection Tasks finish
  through the Runtime before releasing the server object.
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
