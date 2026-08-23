# Task Lifecycle

Runtime owns all transitions below. A Task is created NEW, becomes
RUNNABLE when spawned or woken, RUNNING while an M executes it, and
WAITING or SLEEPING while it is parked on I/O/synchronization or a
timer. I/O completion, timer expiry, or another Task's wakeup returns it
to RUNNABLE.

```mermaid
stateDiagram-v2
    [*] --> NEW
    NEW --> RUNNABLE
    RUNNABLE --> RUNNING
    RUNNING --> WAITING
    RUNNING --> SLEEPING
    RUNNING --> DONE
    RUNNING --> CANCELLED
    WAITING --> RUNNABLE
    SLEEPING --> RUNNABLE
    DONE --> [*]
    CANCELLED --> [*]
```

State semantics:

- `NEW`: task function and arguments assigned, not yet placed in a queue.
- `RUNNABLE`: queued on a P-local queue, the global queue, or stolen by
  another P.
- `RUNNING`: executing on exactly one M; this is the only state where a
  fiber context switch can occur.
- `WAITING`: parked on an I/O Request or synchronization primitive;
  ownership of the wait remains with Runtime.
- `SLEEPING`: parked on a per-P timer heap entry until deadline.
- `DONE`: function returned; the Task handle remains queryable until Runtime
  shutdown, when Runtime reclaims the Fiber and stack.
- `CANCELLED`: cancellation observed cooperatively through runtime calls
  and I/O waits; cleanup runs before the state becomes terminal.

Wakeups never move a Task directly to RUNNING; they always go through
`WAITING`/`SLEEPING` to `RUNNABLE` so the scheduler picks the next P
execution. The I/O completion path is documented in
[docs/diagrams/io-completion.md](io-completion.md). Implementation
references: `include/veloco/task.h` and `src/runtime/task.c`.

Task 4 execution sequence:

```mermaid
sequenceDiagram
    participant Host as Runtime root
    participant Q as FIFO runnable queue
    participant G as Task/G
    participant F as Fiber

    Host->>G: vl_spawn(fn, arg)
    G->>Q: NEW -> RUNNABLE, enqueue
    Host->>Q: pop first Task
    Host->>G: RUNNABLE -> RUNNING
    Host->>F: resume
    F->>G: execute fn
    G->>Q: vl_yield: RUNNING -> RUNNABLE, enqueue
    F-->>Host: suspend to Runtime root
    Host->>Q: pop Task
    Host->>G: RUNNABLE -> RUNNING
    Host->>F: resume
    F-->>Host: function returns
    Host->>G: Fiber DONE -> Task DONE
```

Join adds a wait edge instead of queue membership:

```mermaid
flowchart LR
    RUN["Task RUNNING"] --> WAIT["Task WAITING"]
    WAIT -->|"linked to target.waiters"| TARGET["Target Task"]
    TARGET --> DONE["Target DONE or CANCELLED"]
    DONE --> WAKE["wake waiter"]
    WAKE --> READY["Task RUNNABLE"]
    READY --> RUN
```

Task 7 invariants are: a Task has at most one queue membership, executes on
at most one M, and a terminal Task is never enqueued again. Each M enters
only its P's Fiber scheduler. An unstarted Task may be stolen; after Fiber
creation it returns only to its owning P. Completed handles remain queryable
until Runtime shutdown, when all Task and Fiber resources are reclaimed once.

```mermaid
sequenceDiagram
    participant W as Waker Task
    participant R as Runtime mutex
    participant G as Waiting Task/G
    participant E as owner P eventfd
    participant M as owner M
    W->>R: lock and inspect G
    alt G Fiber still executing
        R->>G: wake_pending = true
        G-->>M: yield to root
        M->>G: WAITING -> RUNNABLE, enqueue
    else G already parked
        R->>G: WAITING -> RUNNABLE, enqueue for last_p
    end
    W->>E: write(1)
    E-->>M: poll wakes
    M->>G: RUNNABLE -> RUNNING
```

Task 5 implements the I/O branch: a Task-bound `vl_io_submit` changes
RUNNING to WAITING and suspends its Fiber. Runtime polls epoll at a scheduler
boundary; the matching Completion writes the Request result, changes WAITING
to RUNNABLE, and appends the Task to the same FIFO queue.

## Task-to-Fiber boundary

Task state describes scheduler eligibility; Fiber state describes whether a
saved CPU context can be entered. They are related but deliberately not the
same state machine.

```mermaid
flowchart LR
    Q["Task/G: RUNNABLE"] --> R["Task/G: RUNNING"]
    R --> F["Fiber: RUNNING"]
    F -->|"yield to runtime"| S["Fiber: SUSPENDED"]
    S -->|"scheduler resumes task"| F
    F -->|"function returns"| D["Fiber: DONE"]
    D --> TD["Task/G: DONE"]
    R -->|"park on I/O or timer"| W["Task/G: WAITING or SLEEPING"]
```

Task 2 implements the Fiber side and its explicit scheduler handle. Task 4
owns the Task/G transitions and maps a parked Task to its suspended Fiber. A
fiber yield alone does not choose a P, enqueue a Task, or represent an I/O
wait.
