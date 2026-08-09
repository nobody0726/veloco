# I/O Completion

The primary path submits one operation per I/O Request, parks the
owning Task, and reaps the kernel completion. A Generation token and a
Task-state check prevent a stale CQE from waking a reused fd or Task.
The first backend is a single io_uring ring with one dedicated I/O
worker; epoll uses the same completion result contract through the
Backend interface.

```mermaid
sequenceDiagram
    autonumber
    participant G as Task/G
    participant R as Runtime
    participant B as Backend (io_uring)
    participant K as Linux kernel

    G->>R: vl_io_submit(request, op, fd, buffer, generation)
    R->>B: enqueue SQE
    B->>K: io_uring_submit / SQE
    K-->>B: operation completion
    B-->>R: reap CQE
    R->>R: validate generation and Task state
    R-->>G: store result, mark RUNNABLE
    G->>G: resume with completion result
```

Cancellation is cooperative and buffer-safe: the runtime submits a
cancel SQE and only releases the request and buffer after the matching
completion has been discarded or matched.

```mermaid
sequenceDiagram
    autonumber
    participant G as Task/G
    participant R as Runtime
    participant B as Backend (io_uring)
    participant K as Linux kernel

    G->>R: vl_io_cancel(request)
    R->>B: enqueue cancel SQE
    B->>K: IORING_OP_ASYNC_CANCEL
    K-->>B: cancel completion
    B-->>R: reap cancel CQE
    R->>R: confirm cancellation, release request/buffer
    R-->>G: wake Task with cancelled result
```

Invariants carried by these paths:

1. The request and its buffer stay valid until completion or confirmed
   cancellation.
2. A completion is dropped only when it belongs to a confirmed
   cancellation; otherwise it must be matched to its Task.
3. Wakeup always transitions `WAITING -> RUNNABLE`, never directly to
   `RUNNING`.

Implementation references: `include/veloco/io.h`, `src/net/backend.c`,
`src/net/epoll_backend.c`, and `src/net/uring_backend.c` (Tasks 5-6).
The epoll fallback implements the same Backend contract with readiness
events instead of SQE/CQE.
