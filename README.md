# Veloco

Veloco is a Linux x86_64 and Linux arm64 C runtime plus HTTP/1.1 server.
It rebuilds the fiber, coroutine, and event-loop foundations of the
existing `ef/` project, then extends them with a G/P/M scheduler, a
Go/TCMalloc-inspired P-local allocator, io_uring-first asynchronous I/O,
and an HTTP/1.1 server. `ef/` is treated as a black-box behavior and
benchmark reference only; no `ef/` source, headers, assembly, utility
code, or tests are copied into Veloco.

## Supported environment

- Linux on x86_64 (`linux/amd64`) and arm64 (`linux/arm64`)
- C11/GNU11 with gcc or clang
- CMake >= 3.22 with the Ninja generator
- pthreads, C11 atomics, `mmap`/`mprotect`
- epoll fallback backend; io_uring requires `liburing-dev`
- Portability to other operating systems or CPU families is out of
  scope for the first release

## Repository layout

- `docs/superpowers/specs/2026-08-09-veloco-runtime-design.md` - approved
  design
- `docs/superpowers/plans/2026-08-09-veloco-implementation-plan.md` -
  task-by-task implementation plan
- `docs/domain/` - ubiquitous language and bounded contexts
- `docs/architecture/` - implemented runtime contracts, ABI layouts,
  ownership rules, and measured evidence
- `docs/diagrams/` - system, Task lifecycle, I/O completion, and
  allocator diagrams
- `docker/` - development and runtime container images
- `deploy/` - local Compose smoke deployment
- `scripts/` - host bootstrap and CI entry points

## Setup

Check the native host before building:

```bash
./scripts/bootstrap-dev.sh        # Linux x86_64/arm64, CMake, compiler, Ninja, pthread, liburing
./scripts/bootstrap-dev.sh epoll  # skip the liburing check for epoll-only work
```

The development container is pinned to Ubuntu 24.04 and can be built for
either supported Linux platform:

```bash
docker build --platform linux/amd64 -f docker/dev.Dockerfile -t veloco-dev .
docker run --rm -it --platform linux/amd64 -v "$PWD:/workspace" -w /workspace veloco-dev

docker build --platform linux/arm64 -f docker/dev.Dockerfile -t veloco-dev:arm64 .
docker run --rm -it --platform linux/arm64 -v "$PWD:/workspace" -w /workspace veloco-dev:arm64
```

Use native Linux x86_64 or native Linux arm64 for performance
measurements. Cross-platform or emulated container runs are useful for
correctness checks only and must not be mixed with native benchmark
baselines.

## Build and test

List presets and run the one-command CI entry point:

```bash
cmake --list-presets
./scripts/ci.sh dev
./scripts/ci.sh uring
./scripts/ci.sh asan
```

Or run each step explicitly:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

Presets `dev`, `epoll`, `uring`, `asan`, `ubsan`, and `tsan` write
architecture-specific build trees such as `build/x86_64/dev` and
`build/aarch64/dev`; logs go under `build/artifacts/<name>`. The
`uring` preset requires liburing. `scripts/ci.sh` sets
`VELOCO_BUILD_PROCESSOR` automatically; set it yourself when invoking
raw `cmake --preset` commands directly.

Task 0 deliberately does not create `CMakeLists.txt`; the configure,
build, and CTest commands above become executable when Task 1 adds the
root `CMakeLists.txt`.

## Run

The HTTP server binary is expected from Task 8. Once an architecture
specific binary such as `build/x86_64/uring/veloco-httpd` or
`build/aarch64/uring/veloco-httpd` exists, a local smoke deployment runs
from `deploy/compose.yaml`:

```bash
docker compose -f deploy/compose.yaml up --build
VELOCO_HOST_PORT=18080 docker compose -f deploy/compose.yaml up --build
```

The runtime image can also be built directly for either supported
platform:

```bash
docker build --platform linux/amd64 \
  --build-arg VELOCO_HTTPD_BINARY=build/x86_64/uring/veloco-httpd \
  -f docker/runtime.Dockerfile -t veloco-httpd:local .

docker build --platform linux/arm64 \
  --build-arg VELOCO_HTTPD_BINARY=build/aarch64/uring/veloco-httpd \
  -f docker/runtime.Dockerfile -t veloco-httpd:arm64 .
```

## CI and release

- `.github/workflows/ci.yml` - x86_64 gcc/clang epoll, gcc io_uring,
  gcc ASan/UBSan, clang TSan, optional native arm64 validation through a
  self-hosted Linux ARM64 runner, and a Docker runtime smoke job that is
  skipped until the binary exists
- `.github/workflows/release.yml` - version-tag builds, uploads
  architecture-labeled Linux binaries, and publishes runtime images to a
  configurable registry

## Current status

Tasks 0-2 of the implementation plan are complete. The repository now
contains the reproducible environment, CMake/test baseline, and independently
implemented stackful fibers for Linux x86_64 and arm64 with guarded lazy
stacks, sanitizer integration, ABI tests, diagrams, and a repeatable context
switch benchmark. Task 3 adds the single-thread allocator.
