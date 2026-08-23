# Veloco Ubiquitous Language

This glossary is the shared vocabulary for all Veloco implementation
tasks. Each term is owned by exactly one bounded context (see
[docs/domain/bounded-contexts.md](bounded-contexts.md)) and maps to a
concrete header or source file in the plan
([docs/superpowers/plans/2026-08-09-veloco-implementation-plan.md](../superpowers/plans/2026-08-09-veloco-implementation-plan.md)).
Terms whose implementation lands in a later task list that task; nothing
here is a placeholder.

## Canonical terms

| Term | Definition | Owner | Implementation reference |
| --- | --- | --- | --- |
| Fiber | P-affine stackful execution context with READY/RUNNING/SUSPENDED/DONE state and a reserved, lazily mapped, guarded stack; the mechanism of context switching, not a scheduling policy. | Runtime | `include/veloco/fiber.h`, `src/fiber/fiber.c`, `src/fiber/fiber_x86_64.S`, `src/fiber/fiber_aarch64.S` |
| Task/G | User-visible schedulable coroutine. A G owns a Fiber, lifecycle state, function/argument, queue membership, join waiters, and its last P. An unstarted G can be stolen; after Fiber creation it remains on that P. Task and G are synonyms. | Runtime | `include/veloco/task.h`, `src/runtime/task.c`, `src/runtime/scheduler.c` |
| P | Logical processor with one owner M, a Chase-Lev runnable deque, Fiber scheduler, and counters. Timer and allocator-cache ownership are added in later Task 7 steps. | Runtime, Memory | `src/runtime/worker.c`, `src/runtime/run_queue.c`, `src/memory/valloc.c` |
| M | Worker thread that owns one fixed P. M0 is the Runtime owner thread; remaining M instances are persistent pthreads parked through eventfd. | Runtime | `src/runtime/worker.c` |
| I/O Request | Caller-owned operation descriptor containing op, fd, buffer/address/timeout data, generation, and Task identity. The selected backend borrows it until exactly one Completion is consumed or its handle is destroyed. | Async I/O | `include/veloco/io.h`, `src/net/backend.c`, `src/net/epoll_backend.c`, `src/net/uring_backend.c` (Tasks 5-6) |
| Span | Page range divided into objects of one size class, with central/cache free lists, active/free counters, and debug metadata. | Memory | `src/memory/span.c` (Task 3) |
| Arena | Request-lifetime allocation region whose mmap-backed blocks are released as a unit when reset or when the owning HTTP request completes. | Memory, HTTP | `src/memory/arena.c` (Task 3) |
| Connection | HTTP socket, parser state, request arena, and the connection Task that owns them. | HTTP | `include/veloco/http.h`, `src/http/http_connection.c` (Task 8) |
| Task mutex | Non-recursive Runtime-bound mutex that transfers ownership to the oldest parked Task instead of blocking an M. | Runtime | `include/veloco/sync.h`, `src/sync/task_mutex.c` |
| Semaphore | Runtime-bound permit counter with a FIFO Task waiter queue. | Runtime | `include/veloco/sync.h`, `src/sync/semaphore.c` |
| Wait group | Runtime-bound counter whose zero transition wakes all parked Tasks. | Runtime | `include/veloco/sync.h`, `src/sync/wait_group.c` |
| Channel | Runtime-bound bounded FIFO message queue; capacity zero is a rendezvous and close wakes blocked senders/receivers. | Runtime | `include/veloco/sync.h`, `src/sync/channel.c` |
| Timer | Runtime-bound monotonic deadline node owned by a Task's P-local min-heap; expiry or cancellation wakes the Task exactly once. | Runtime | `include/veloco/timer.h`, `src/time/timer.c`, `src/time/timer_heap.c` |

## Secondary terms

| Term | Definition | Owner |
| --- | --- | --- |
| SizeClass | Fixed object size bucket that requests are rounded up to. | Memory |
| PageHeap | mmap-backed layer that acquires and releases page ranges. | Memory |
| Cache | Single-thread fast path in Task 3, later owned by P; refilled from and drained to the central free list in batches. | Memory |
| Central free list | Task 3's single-thread span list; it becomes lock-protected and P-shared in the multi-worker milestone. | Memory |
| Backend | Abstraction over io_uring and epoll that exposes operations and completion results, not readiness types. | Async I/O |
| Black-box Baseline | External behavior, smoke output, and benchmark data observed from `ef/` without copying or translating its implementation or tests. | Runtime |
| Ring Worker | Dedicated pthread that exclusively owns one io_uring instance, translates synchronized commands into SQEs, and publishes backend-neutral Completions from CQEs. | Async I/O |
| SQE | Submission queue entry prepared only by the Ring Worker; its `user_data` identifies one internal operation record. | Async I/O |
| CQE | Kernel completion consumed only by the Ring Worker. The original operation CQE produces at most one public Completion; an async-cancel CQE is internal. | Async I/O |
| Generation | Token attached to an I/O Request so a stale completion cannot wake a reused fd or Task. | Async I/O |
| I/O Completion | Backend-neutral result preserving the exact Request, Task identity, and fd generation; epoll produces it after readiness adaptation and io_uring produces it from the original operation CQE. Runtime consumes it and wakes a Task-bound request. | Async I/O |
| Parser | Incremental HTTP/1.1 request-line and header parser with size limits. | HTTP |
| Router | Maps request method and path to handlers. | HTTP |
| Response | HTTP/1.1 response with status, headers, and body encoding (Content-Length or chunked). | HTTP |

## Invariants

1. A Task/G never executes simultaneously on two M threads.
2. The allocator cache belongs to P, not M. An unstarted Task may be stolen,
   but a Task with a live stackful Fiber remains bound to its first P.
3. An object freed by a different P enters the owning P's remote-free
   queue and is drained at scheduler safe points or when that P's local
   cache needs maintenance.
4. An I/O Request's buffer and metadata remain valid until completion or
   confirmed cancellation; no completion is silently dropped.
5. Every completion is checked against the request generation and the
   Task state before the Task is woken.
6. Cancellation is cooperative and never frees an I/O buffer before the
   original operation CQE resolves ownership. An async-cancel CQE never
   creates a second user-visible Completion.
7. Allocation is explicit-free only; there is no garbage collector.
8. Waiting for I/O, a timer, or a synchronization primitive parks the
   Task and keeps the M available to other Tasks.
9. Shutdown rejects new Tasks, stops accepting, cancels or drains active
   I/O, wakes waiting Tasks, then joins workers and releases runtime
   state.

## Explicitly deferred vocabulary

Dynamic M creation, preemptive scheduling, span coalescing, P-sharded
io_uring rings, registered files/buffers, multishot operations, buffer
rings, and SQPOLL are experiments deferred until their milestone gates
pass. They are not part of the first release and are not referenced as
if they existed.
