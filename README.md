# Veloco

Veloco is a Linux x86_64 C runtime and HTTP/1.1 server project built by
reconstructing and extending the existing `ef` coroutine framework.

Current contents:

- `docs/superpowers/specs/2026-08-09-veloco-runtime-design.md`
- `docs/superpowers/plans/2026-08-09-veloco-implementation-plan.md`

Planned direction:

- new implementation, no reuse of `ef/` source or tests
- io_uring-first asynchronous I/O
- G/P/M scheduling
- P-local allocator inspired by Go and TCMalloc
- HTTP/1.1 server with benchmarks and CI

