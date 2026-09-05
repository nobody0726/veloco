# Veloco Benchmark Evidence

Task 9 records repeatable measurements from the same repository state that
introduced the benchmark harnesses. The measurements below are correctness
benchmarks, not native performance claims.

## HTTP workload

Command:

```bash
./build/$ARCH/epoll/veloco_bench_http --backend epoll --workers 1 \
  --requests 500 --concurrency 16
```

Observed results:

| Architecture | Duration ns | QPS | p50 ns | p95 ns | p99 ns | Task switches | I/O submits | I/O completions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| x86_64 | 15,958,959 | 31,330 | 398,292 | 465,709 | 2,940,917 | 3,500 | 1,500 | 1,500 |
| aarch64 | 6,711,125 | 74,503 | 188,834 | 264,250 | 414,916 | 3,500 | 1,500 | 1,500 |

Observed counter tail:

```text
parks=1500
io_cancellations=0
active_connections=0
```

`io_uring` execution in Docker Desktop was not available for this
benchmark slice; the binary returned `-5` from `bench_http`, which the
current HTTP path treats as a runtime invalid-state failure.

Soak run:

```bash
./build/$ARCH/epoll/veloco_bench_http --backend epoll --workers 1 \
  --requests 1000000 --concurrency 16
```

Observed result on `aarch64`:

- `duration_ns=11953574631`
- `qps=83657`
- `task_switches=7000000`
- `io_submissions=3000000`
- `io_completions=3000000`

## Allocator workload

Command:

```bash
./build/$ARCH/epoll/bench_alloc
```

Observed results:

| Architecture | Operations/s | Cache hits | Central refills | Mapped bytes |
| --- | ---: | ---: | ---: | ---: |
| x86_64 | 36,848,644 | 999,831 | 169 | 2,646,016 |
| aarch64 | 44,395,938 | 999,833 | 167 | 2,506,752 |
