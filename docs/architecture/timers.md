# Per-P Timers

Timers use a monotonic nanosecond deadline and a binary min-heap on the P
that owns the sleeping Task. The Runtime mutex protects heap mutation because
`vl_timer_cancel` may be called by another Task while the owner M is polling.

```mermaid
classDiagram
    class P {
        +TimerHeap timers
        +size_t timer_count
    }
    class Timer {
        +Runtime runtime
        +Task owner
        +P p
        +uint64 deadline_ns
        +bool active
    }
    class Task {
        +SLEEPING state
        +timer_result
    }
    P *-- Timer : min-heap
    Timer --> Task
```

## Lifecycle

```mermaid
sequenceDiagram
    participant G as Task/G
    participant R as Runtime mutex
    participant H as P timer heap
    participant M as Owner M
    G->>R: RUNNING -> SLEEPING
    R->>H: insert deadline
    G->>M: yield Fiber
    M->>M: poll until heap root deadline
    M->>H: pop expired node
    H->>R: SLEEPING -> RUNNABLE
    R->>M: eventfd wake
    M->>G: resume with VL_OK
    Note over R,H: cancel removes the node and wakes with VL_ERROR_CANCELLED
```

An expired or cancelled node is removed before its Task is woken, so one
timer can produce at most one wake. A zero-delay timer still yields once and
is expired at the next scheduler boundary. `vl_sleep_ns` uses a temporary
timer handle; explicit handles allow another Task to cancel a wait.

Timer heap tests cover ordering and arbitrary removal. Runtime tests cover a
monotonic deadline, cross-Task cancellation, and exactly-once wake behavior.
