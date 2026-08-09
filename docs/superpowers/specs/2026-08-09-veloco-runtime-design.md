# Veloco Runtime and HTTP Server Design

Date: 2026-08-09

Status: Approved design for implementation planning

## 1. Project Positioning

Veloco is a Linux x86_64 C runtime and HTTP/1.1 server built by
reconstructing and extending the existing `ef` coroutine framework.

The project has three connected goals:

1. Rebuild the existing fiber, coroutine, and event-loop foundations so
   their behavior and trade-offs are understood at implementation level.
2. Extend the runtime horizontally with G/P/M scheduling, work stealing,
   asynchronous I/O, synchronization primitives, timers, and a
   Go/tcmalloc-inspired memory allocator.
3. Extend the system vertically into a usable HTTP/1.1 server with
   benchmarks, diagnostics, failure tests, and engineering documentation.

The project is intentionally Linux x86_64 first. Portability is not part
of the first release.

The term `tmalloc` is interpreted as a request for a simplified
TCMalloc-like allocator. The allocator uses a similar layered topology,
but it does not implement garbage collection.

## 2. Scope

### In scope

- x86_64 fiber context switching and guarded, growable stacks
- A standalone Task abstraction above fibers
- Cooperative scheduling
- Fixed-size G/P/M runtime with local queues and work stealing
- Linux `io_uring` as the primary asynchronous I/O backend
- `epoll` as a fallback backend and performance baseline
- `eventfd` for cross-thread wakeups
- Asynchronous accept, recv, send, connect, and timeout operations
- P-local size-class allocator, central free lists, and page heap
- Explicit allocation and release through `vl_malloc` and `vl_free`
- Request arenas and fixed-object pools
- Task cancellation, deadlines, and graceful shutdown
- HTTP/1.1 request parsing, Keep-Alive, Content-Length, and chunked
  responses
- Router, middleware, connection limits, and backpressure
- Unit, integration, stress, sanitizer, and benchmark suites

### Not in the first release

- kqueue and Solaris event-port backends
- HTTP/2 and HTTP/3
- TLS
- A tracing garbage collector
- NUMA-aware allocation
- Distributed clustering
- General asynchronous disk I/O
- Experimental preemptive scheduling

These may become later projects only after the first release has stable
correctness and benchmark evidence.

## 3. Existing `ef` Baseline

The current `ef` codebase contains:

- `fiber.c/h`: mmap-backed fiber stacks, guard pages, stack expansion,
  and assembly context switching
- `coroutine.c/h`: a coroutine pool built on top of fibers
- `framework.c/h`: one event loop, connection queues, coroutine-backed
  socket wrappers, and graceful stop handling
- `poll.c`, `epoll.c`, `epollet.c`, `kqueue.c`, and `port.c`: poller
  implementations behind a common interface
- `util/list.h`: intrusive linked-list utilities

The baseline is single-threaded. It uses readiness-style nonblocking
socket operations: a coroutine attempts an operation, yields on
`EAGAIN`, and resumes after a poller event. Global runtime state and
manual object allocation are currently mixed into framework code.

Veloco will preserve the useful fiber and poller lessons while moving
ownership, scheduling, I/O requests, and memory lifecycle into explicit
modules.

## 4. Architecture

```text
HTTP Server / Router / Middleware
                 |
      Async I/O API / Sync / Context
                 |
              Runtime
       +---------+---------+
       |                   |
   G/P/M Scheduler       Memory
       |           P-local -> Central -> Page Heap
       |
 I/O Backend Interface
       +-------------------------+
       |                         |
  io_uring primary          epoll fallback
       |
 Linux x86_64 / pthread / atomics / mmap / sockets
```

Suggested repository layout:

```text
veloco/
├── include/veloco/
│   ├── runtime.h
│   ├── task.h
│   ├── io.h
│   ├── sync.h
│   ├── timer.h
│   ├── memory.h
│   ├── context.h
│   └── http.h
├── src/
│   ├── fiber/
│   ├── runtime/
│   ├── net/
│   ├── memory/
│   ├── sync/
│   ├── time/
│   └── http/
├── examples/
├── tests/
├── benchmarks/
├── docs/
└── CMakeLists.txt
```

The public API must not expose epoll or io_uring types. Applications use
the async I/O API, and the runtime selects the backend at initialization.

## 5. Runtime Model

### G: Task

A G is a runnable or waiting logical task. It owns:

- a fiber context and stack
- a task function and argument
- a lifecycle state
- a parent context
- a cancellation/deadline reference
- an optional current I/O request
- scheduling metadata and statistics

The lifecycle is:

```text
NEW -> RUNNABLE -> RUNNING -> WAITING
                         -> SLEEPING
                         -> DONE
                         -> CANCELLED
```

One G must never execute simultaneously on two M threads.

### P: Logical Processor

A P owns the resources needed to execute tasks efficiently:

- local runnable queue
- allocator cache
- timer queue
- runtime counters
- remote-free queue

The first scheduler uses a bounded Chase-Lev-style work-stealing queue
with a protected overflow/global queue. The queue memory-ordering rules
must be documented and covered by stress tests.

### M: Worker Thread

An M is a pthread that executes tasks while holding a P. It may detach
from a P when it must perform runtime-level waiting, allowing another M
to continue running that P.

The first implementation uses a fixed worker count. Dynamic M creation
and preemptive scheduling are deliberately deferred.

### Scheduling Rules

1. Run the current P's local queue first.
2. Check the global queue when the local queue is empty.
3. Steal a batch from another P when no local work is available.
4. Sleep an idle M and wake it through `eventfd`.
5. When a Task waits for I/O, a timer, or a runtime synchronization
   primitive, mark it waiting and run another Task.
6. Shutdown stops new work, closes listeners, drains or cancels active
   operations, and joins all M threads.

The first release is cooperative. A Task must yield through runtime APIs
before it can be rescheduled.

## 6. Memory Allocator

The allocator is a first-class subsystem named `valloc` internally.

### Topology

```text
vl_malloc / vl_free
          |
      P-local cache
          |
   Central free list
          |
       Page heap
          |
    mmap / munmap
```

### Small objects

Small objects are rounded to configured size classes. A P-local cache
serves the fast path without a central lock. Refill and drain happen in
batches to amortize synchronization.

The initial size-class table is fixed and documented. It must cover
common runtime and HTTP objects without attempting to optimize every
possible allocation pattern.

### Central free lists and spans

The central layer manages spans grouped by size class. A span contains
objects of one class and tracks:

- page range
- size class
- object count
- free count
- owning central list
- debug state

The page heap obtains and returns page ranges through `mmap` and
`munmap`, and may later add span coalescing.

### Ownership and cross-P frees

The cache belongs to P, not M. This keeps allocator locality aligned with
the scheduler because a Task may migrate between worker threads.

An object freed by a different P enters a remote-free queue. The owning P
drains that queue at scheduler safe points or when its local cache needs
maintenance. The implementation must not assume that a Task remains on
the same M.

### Allocation classes

```text
vl_malloc/vl_free       general explicit allocation
vl_arena_alloc          request-lifetime batch allocation
vl_pool_alloc           fixed-size object pool
fiber_stack_alloc       guarded mmap-backed fiber stack
```

Fiber stacks remain separate from the normal allocator because they need
guard pages, lazy mapping, and stack growth.

### Debug and metrics

Debug builds add canaries, poison patterns, allocation metadata, and
double-free detection. Runtime statistics include:

- allocation and free counts
- bytes allocated and released
- cache hits and refills
- central lock contention
- mmap calls
- active spans
- cross-P frees
- estimated internal fragmentation

The allocator is explicit-free only. It does not include a garbage
collector.

## 7. Asynchronous I/O

### Backend policy

`io_uring` is the primary Linux backend. `epoll` remains a separate
backend for baseline comparison and fallback. The backend interface
exposes operations and completion results, not readiness-specific types.

### io_uring operation model

The runtime submits concrete operations:

- `accept`
- `recv`
- `send`
- `connect`
- timeout
- cancellation

Each operation is represented by a `vl_io_request_t` containing its Task,
operation type, fd, buffer, length, generation token, and completion
result.

```text
Task
  -> create io request
  -> submit SQE
  -> WAITING
  -> CQE arrives
  -> validate generation and state
  -> store result
  -> RUNNABLE
```

Buffers and request metadata remain valid until completion or confirmed
cancellation.

### Rollout

1. Build a single-ring backend with one dedicated I/O worker.
2. Cover single-shot accept, recv, send, connect, and timeout.
3. Add cancellation and close-before-completion handling.
4. Add batched SQE/CQE processing.
5. Add P-sharded rings after the correctness and benchmark baseline is
   stable.
6. Explore registered files, registered buffers, multishot operations,
   buffer rings, and SQPOLL as optional optimization experiments.

The first implementation may use system `liburing` to reduce ring setup
boilerplate. The repository must document the mapping between the
library calls and the underlying io_uring UAPI. A small raw-syscall
learning example is optional and non-gating; it must not delay the
library-backed backend or change the release acceptance criteria.

## 8. Synchronization, Timers, and Context

Runtime synchronization primitives must park Tasks rather than block M:

- mutex
- semaphore
- wait group
- channel
- sleep
- deadline timer
- cancellation context

The initial timer implementation uses a per-P min-heap. Timer wakeups
move Tasks back to RUNNABLE. Later, a timer wheel may be evaluated only
if benchmark data shows that the heap is a bottleneck.

Cancellation is cooperative. A cancelled Task observes cancellation
through runtime calls and I/O waits. A cancellation request must not
free an I/O buffer before the corresponding completion has been
discarded or matched.

## 9. HTTP/1.1 Server

The HTTP layer uses only the public Runtime API.

Required first-release features:

- incremental request parsing
- request-line and header size limits
- Content-Length
- Keep-Alive
- chunked responses
- partial reads and writes
- read/write deadlines
- connection limits
- backpressure
- router
- middleware
- graceful shutdown

The request path is:

```text
accept
  -> spawn Task
  -> allocate connection/request from P-local allocator or arena
  -> async recv
  -> parse HTTP request
  -> router/middleware
  -> async send
  -> keep connection or close
  -> release request arena
```

HTTP/2, TLS, and application-specific business logic are not required for
the first release.

## 10. Vertical Learning Artifacts

Each layer must produce an explanation and an executable proof:

| Layer | Explanation | Proof |
|---|---|---|
| Fiber | ABI, stack, guard page, context switch | fiber tests and switch benchmark |
| Task | lifecycle and wait states | state-machine tests |
| Scheduler | G/P/M and work stealing | multi-worker stress test |
| I/O | SQE/CQE lifecycle and cancellation | completion and race tests |
| Allocator | size class, span, cache, page heap | allocator benchmark and fragmentation report |
| HTTP | parser and connection lifecycle | protocol and load tests |
| Runtime | shutdown and failure behavior | fault-injection report |

This turns the project into a body of evidence rather than a collection
of source files.

## 11. Milestones

### H0: Baseline

- Reproduce the original `ef` build and HTTP behavior.
- Add a repeatable benchmark command.
- Add tests around existing fiber and coroutine behavior.
- Record the baseline before refactoring.

### H1: Single-thread Runtime

- Separate Task from connection objects.
- Introduce explicit Task states and run queues.
- Preserve a working socket API.
- Add runtime shutdown and task statistics.

### H2: First Allocator

- Implement fixed size classes.
- Implement spans and page heap.
- Add single-thread tests and statistics.
- Migrate Task and HTTP object allocation.

### H3: G/P/M Runtime

- Add fixed worker pool.
- Add per-P queues and work stealing.
- Add eventfd wakeups.
- Add P-local allocator caches and remote-free queues.

### H4: io_uring

- Add backend interface.
- Implement one-ring, one-I/O-worker path.
- Implement accept, recv, send, connect, timeout, and cancellation.
- Compare against epoll using the same HTTP workload.

### H5: HTTP Product

- Implement parser, Keep-Alive, limits, router, middleware, and graceful
  shutdown.
- Add a static response and a dynamic endpoint that reports runtime and
  allocator statistics.

### H6: Evidence

- Run sanitizer and long-duration stress tests.
- Collect perf data and latency distributions.
- Publish architecture, benchmark, and failure-analysis documents.

## 12. Error Handling and Shutdown

Every asynchronous request has a generation/token check so a stale
completion cannot wake a newly reused fd or Task.

The shutdown sequence is:

```text
stop accepting
  -> reject new tasks
  -> cancel or drain I/O
  -> wake waiting tasks
  -> close active connections
  -> drain allocator remote frees
  -> join I/O worker and M workers
  -> release rings, spans, and runtime state
```

Error paths must preserve ownership. In particular, an I/O completion
must either release its request and buffer or transfer them to a live
Task; no completion may be silently dropped.

## 13. Test and Benchmark Plan

### Correctness

- compiler warnings as errors
- unit tests for fiber, task, queue, allocator, timer, and parser
- integration tests for HTTP and backend selection
- stress tests for scheduler and allocator
- close, timeout, cancellation, and shutdown race tests

### Dynamic analysis

- AddressSanitizer
- UndefinedBehaviorSanitizer
- ThreadSanitizer where compatible with the context-switching code
- debug canaries and poison memory
- gdb and core-dump inspection for fiber failures

### Performance comparison

Keep four repeatable configurations:

```text
A. original ef + epoll
B. Veloco single-thread + epoll
C. Veloco single-thread + io_uring
D. Veloco multi-thread + io_uring + valloc
```

Record QPS, P50/P95/P99 latency, CPU, RSS, context switches, system
calls, allocations per request, allocator cache hit rate, work-stealing
count, and SQE/CQE batch sizes.

### Acceptance criteria

The two-to-three-month release is complete when:

1. Linux x86_64 builds reproducibly.
2. The HTTP server supports concurrent connections, Keep-Alive,
   deadlines, and graceful shutdown.
3. Waiting for I/O or a runtime synchronization primitive does not block
   the whole worker.
4. The io_uring path covers accept, recv, send, connect, timeout, and
   cancellation.
5. `valloc` supports small-object caching, large allocations,
   cross-P frees, and statistics.
6. The multi-thread version shows a measured throughput improvement over
   the single-thread version on the same machine and workload.
7. Stress and sanitizer runs have no known memory errors.
8. The repository contains comparative benchmark data and design notes.

## 14. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| io_uring unavailable or restricted | Keep epoll fallback and document kernel requirements |
| stale CQE after close or cancel | Generation tokens and explicit request ownership |
| work-stealing memory-order bugs | Small queue API, stress tests, sanitizer runs, conservative fences |
| allocator fragmentation | Span statistics, size-class report, benchmark against libc |
| scheduler starvation | Local queue limits, periodic global-queue checks, metrics |
| scope expansion | Defer TLS, HTTP/2, GC, NUMA, and distributed features |
| assembly and sanitizer interaction | Isolate fiber assembly and test runtime behavior separately |

## 15. Resume-Facing Summary

> 基于 C 语言从零重构并扩展协程网络框架，设计实现 G/P/M 多线程调度模型、P-local 分层内存分配器和 io_uring 异步 I/O 后端，完成 HTTP/1.1 Server，并通过压力测试、perf 和 sanitizer 验证并发正确性与性能。

The resume claim must be updated with measured numbers only after the
benchmark suite has produced reproducible results.
