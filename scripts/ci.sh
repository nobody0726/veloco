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

artifact_dir="build/artifacts/${preset}"
rm -rf "${artifact_dir}"
mkdir -p "${artifact_dir}"

run_step() {
  local name="$1"
  local log="$2"
  shift 2
  echo "==> ${name}: $*"
  if "$@" >"${log}" 2>&1; then
    return 0
  fi
  local status=$?
  echo "error: ${name} failed (exit ${status}); log: ${log}" >&2
  sed -n '1,200p' "${log}" >&2 || true
  exit "${status}"
}

run_step configure "${artifact_dir}/configure.log" cmake --preset "${preset}"
run_step build "${artifact_dir}/build.log" cmake --build --preset "${preset}" --parallel

if [ -f "build/${preset}/CTestTestfile.cmake" ]; then
  run_step test "${artifact_dir}/test.log" ctest --preset "${preset}" --output-on-failure
else
  echo "note: build/${preset} has no CTestTestfile.cmake; ctest starts in Task 1" | tee "${artifact_dir}/test.log"
fi

if [ -f "build/${preset}/CMakeCache.txt" ]; then
  cp "build/${preset}/CMakeCache.txt" "${artifact_dir}/CMakeCache.txt"
fi
if [ -f "build/${preset}/Testing/Temporary/LastTest.log" ]; then
  cp "build/${preset}/Testing/Temporary/LastTest.log" "${artifact_dir}/LastTest.log"
fi

echo "ci complete for preset ${preset}; logs in ${artifact_dir}"
