# Veloco Baseline Measurements

Date: 2026-08-10

Status: Task 1 black-box baseline. The `ef/` tree is observed only as an
external behavior and benchmark reference; no `ef` source, headers,
assembly, utility code, or tests are copied into Veloco.

## Original `ef` epoll smoke baseline

Environment:

```text
Host command environment: Docker Desktop on macOS arm64
Container platform: linux/amd64
Container kernel: Linux 6.12.76-linuxkit
Compiler: gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
Reported CPUs: 10
Configured ef worker count: 16
```

Build command:

```bash
docker run --rm --platform linux/amd64 \
  -v "/Users/wangfeng/workspace/veloco:/workspace" \
  -w /workspace/ef \
  veloco-dev \
  bash -lc 'make clean >/tmp/ef-clean.log 2>&1 || true;
            make prog_epoll SHELL=/bin/bash'
```

Smoke command:

```bash
./prog_epoll &
curl --silent --fail --max-time 1 \
  -H "Connection: Close" \
  http://127.0.0.1:8080/
```

Observed response body:

```text
Welcome to the EFramework!
```

Lightweight concurrency check:

```bash
seq 1 100 | xargs -n1 -P10 sh -c \
  'curl --silent --fail --max-time 5 \
     -H "Connection: Close" \
     http://127.0.0.1:8080/ > "/tmp/ef-curl-${0}.out"'
```

Observed summary:

```text
request_count=100
concurrency=10
successful_responses=100
elapsed_ms=956
approx_rps=104
total_body_bytes=2600
process_cpu_percent_sample=0.8
process_rss_kb_sample=4460
process_vsz_kb_sample=291852
```

## Interpretation

This is a correctness-oriented baseline, not a performance claim. The
original `ef` Makefile builds the amd64 assembly path (`-m64` and
`amd64/fiber.s`), so it cannot be used as a native Linux arm64 baseline
without modifying the reference project. Veloco keeps that limitation in
the benchmark notes instead of changing `ef/`.

The first resume-grade throughput comparison must be rerun on native
Linux hardware with a stable tool such as `ab` or `wrk`, and x86_64 and
arm64 results must be recorded in separate sections.
