#!/usr/bin/env bash
# Verify that the local host can build and test Veloco.
# Usage: ./scripts/bootstrap-dev.sh [dev|epoll|uring|asan|ubsan|tsan]
# The default "dev" check requires liburing because the dev preset enables
# the io_uring backend. Pass "epoll" to validate an epoll-only environment.
set -euo pipefail

target="${1:-dev}"
need_uring=0
case "${target}" in
  dev | --dev | uring | --uring)
    need_uring=1
    ;;
  epoll | --epoll | asan | --asan | ubsan | --ubsan | tsan | --tsan)
    ;;
  *)
    echo "error: unknown argument '${1}'; usage: $0 [dev|epoll|uring|asan|ubsan|tsan]" >&2
    exit 2
    ;;
esac

fail() {
  echo "error: $*" >&2
  exit 1
}

os="$(uname -s 2>/dev/null || true)"
arch="$(uname -m 2>/dev/null || true)"
if [ "${os}" != "Linux" ]; then
  fail "unsupported OS '${os:-unknown}'; Veloco requires Linux x86_64 or arm64"
fi
case "${arch}" in
  x86_64 | amd64)
    canonical_arch="x86_64"
    ;;
  aarch64 | arm64)
    canonical_arch="arm64"
    ;;
  *)
    fail "unsupported architecture '${arch:-unknown}'; Veloco requires x86_64 or arm64"
    ;;
esac

command -v cmake >/dev/null 2>&1 || fail "cmake is not installed; install CMake >= 3.22 (Ubuntu: apt install cmake)"
cmake_version="$(cmake --version | awk '/cmake version/ { print $3; exit }')"
if [ -z "${cmake_version}" ]; then
  fail "cmake is installed but 'cmake --version' did not report a version"
fi
cmake_major="$(printf '%s' "${cmake_version}" | cut -d. -f1)"
cmake_minor="$(printf '%s' "${cmake_version}" | cut -d. -f2)"
if [ "${cmake_major:-0}" -lt 3 ] || { [ "${cmake_major:-0}" -eq 3 ] && [ "${cmake_minor:-0}" -lt 22 ]; }; then
  fail "cmake ${cmake_version} is too old; Veloco presets require CMake >= 3.22"
fi

cc_candidate="${CC:-cc}"
command -v "${cc_candidate}" >/dev/null 2>&1 || fail "C compiler '${cc_candidate}' not found; install gcc or clang (Ubuntu: apt install build-essential)"

command -v ninja >/dev/null 2>&1 || fail "ninja is not installed; Veloco presets use the Ninja generator (Ubuntu: apt install ninja-build)"

tmp_dir="$(mktemp -d)"
trap 'rm -rf "${tmp_dir}"' EXIT

{
  printf '%s\n' \
    '#include <pthread.h>' \
    'int main(void) {' \
    '  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;' \
    '  return pthread_mutex_lock(&mutex);' \
    '}'
} > "${tmp_dir}/pthread_probe.c"
if ! "${cc_candidate}" -pthread "${tmp_dir}/pthread_probe.c" -o "${tmp_dir}/pthread_probe" 2>/dev/null; then
  fail "pthread support is missing; install a pthread-capable toolchain (Ubuntu: apt install libc6-dev build-essential)"
fi

if [ "${need_uring}" -eq 1 ]; then
  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liburing; then
    :
  elif printf '%s\n' \
    '#include <liburing.h>' \
    'int main(void) { return 0; }' | "${cc_candidate}" -x c - -luring -o "${tmp_dir}/uring_probe" 2>/dev/null; then
    :
  else
    fail "liburing development files are missing; install liburing-dev (Ubuntu: apt install liburing-dev)"
  fi
fi

echo "bootstrap ok: Linux ${canonical_arch}, cmake ${cmake_version}, compiler ${cc_candidate}, ninja, pthread"
if [ "${need_uring}" -eq 1 ]; then
  echo "bootstrap ok: liburing development files present"
fi
