#!/usr/bin/env bash
# One-command CI entry point: configure, build, test, and collect logs.
# Usage: ./scripts/ci.sh <preset>
set -euo pipefail

preset="${1:-}"
if [ -z "${preset}" ]; then
  echo "error: usage: $0 <preset>" >&2
  exit 2
fi
case "${preset}" in
  dev | epoll | uring | asan | ubsan | tsan)
    ;;
  *)
    echo "error: unknown preset '${preset}'; expected one of: dev, epoll, uring, asan, ubsan, tsan" >&2
    exit 2
    ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

arch="$(uname -m 2>/dev/null || true)"
case "${arch}" in
  x86_64 | amd64)
    export VELOCO_BUILD_PROCESSOR=x86_64
    ;;
  aarch64 | arm64)
    export VELOCO_BUILD_PROCESSOR=aarch64
    ;;
  *)
    echo "error: unsupported architecture '${arch:-unknown}'; expected x86_64 or arm64" >&2
    exit 2
    ;;
esac

artifact_dir="build/artifacts/${preset}"
rm -rf "${artifact_dir}"
mkdir -p "${artifact_dir}"

run_step() {
  local name="$1"
  local log="$2"
  shift 2
  echo "==> ${name}: $*"
  set +e
  "$@" >"${log}" 2>&1
  local status=$?
  set -e
  if [ "${status}" -eq 0 ]; then
    return 0
  fi
  echo "error: ${name} failed (exit ${status}); log: ${log}" >&2
  sed -n '1,200p' "${log}" >&2 || true
  exit "${status}"
}

run_step configure "${artifact_dir}/configure.log" cmake --preset "${preset}"
run_step build "${artifact_dir}/build.log" cmake --build --preset "${preset}" --parallel
run_step test "${artifact_dir}/test.log" ctest --preset "${preset}" --output-on-failure

cache_file="build/${VELOCO_BUILD_PROCESSOR}/${preset}/CMakeCache.txt"
if [ -f "${cache_file}" ]; then
  cp "${cache_file}" "${artifact_dir}/CMakeCache.txt"
fi
last_test_log="build/${VELOCO_BUILD_PROCESSOR}/${preset}/Testing/Temporary/LastTest.log"
if [ -f "${last_test_log}" ]; then
  cp "${last_test_log}" "${artifact_dir}/LastTest.log"
fi

echo "ci complete for preset ${preset}; logs in ${artifact_dir}"
