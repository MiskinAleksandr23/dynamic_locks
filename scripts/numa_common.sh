#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BENCH_BIN="${BENCH_BIN:-${REPO_ROOT}/build/adaptive_lock_benchmark}"
RESULT_BASE="${RESULT_BASE:-${REPO_ROOT}/BENCHMARK_RESULT/numa}"
OPERATIONS="${OPERATIONS:-2000000}"
REPETITIONS="${REPETITIONS:-3}"
SEED="${SEED:-1}"
ARRAY_SIZE="${ARRAY_SIZE:-1048576}"
SAMPLE_RATE="${SAMPLE_RATE:-64}"
REBUILD_MS="${REBUILD_MS:-30000}"
TRACE_SIZE="${TRACE_SIZE:-1048576}"
NUMA_TIMEOUT_SECONDS="${NUMA_TIMEOUT_SECONDS:-1800}"

make_result_dir() {
  local label="$1"
  mkdir -p "${RESULT_BASE}"
  mktemp -d "${RESULT_BASE}/$(date -u +%Y%m%d_%H%M%S)_${label}_XXXXXX"
}

write_topology() {
  local out_dir="$1"
  {
    date -u
    uname -a
    if command -v lscpu >/dev/null 2>&1; then
      lscpu
    fi
    if command -v numactl >/dev/null 2>&1; then
      numactl --hardware
    else
      echo "numactl not found"
    fi
  } > "${out_dir}/topology.txt" 2>&1
}

run_suite() {
  local out_dir="$1"
  local placement_label="$2"
  local threads="$3"
  shift 3
  local numa_args=("$@")
  local stem="${out_dir}/${placement_label}_${threads}t"
  local cmd=()

  if ((${#numa_args[@]} > 0)); then
    cmd+=(numactl "${numa_args[@]}")
  fi
  cmd+=("${BENCH_BIN}" --threads "${threads}" --operations "${OPERATIONS}"
        --repetitions "${REPETITIONS}" --seed "${SEED}" --scenario all
        --array-size "${ARRAY_SIZE}" --sample-rate "${SAMPLE_RATE}"
        --rebuild-ms "${REBUILD_MS}" --trace-size "${TRACE_SIZE}" --csv "${stem}.csv")
  if [[ -n "${HOT_WIDTH:-}" ]]; then
    cmd+=(--hot-width "${HOT_WIDTH}")
  fi
  if [[ -n "${RANGE_LENGTH:-}" ]]; then
    cmd+=(--range-length "${RANGE_LENGTH}")
  fi

  {
    printf '# command: '
    printf '%q ' "${cmd[@]}"
    printf '\n# timeout: %s seconds for the complete suite\n' "${NUMA_TIMEOUT_SECONDS}"
  } > "${stem}.txt"
  echo "Running all scenarios: ${placement_label}, ${threads} threads; output ${stem}.txt"

  # Keep partial CSV and the failure status; never silently drop a failed suite.
  set +e
  timeout "${NUMA_TIMEOUT_SECONDS}" "${cmd[@]}" 2>&1 | tee -a "${stem}.txt"
  local statuses=("${PIPESTATUS[@]}")
  set -e
  local rc="${statuses[0]}"
  if [[ ${rc} -eq 0 ]]; then
    rc="${statuses[1]}"
  fi
  if [[ ${rc} -eq 0 ]]; then
    echo "OK" > "${stem}.status"
  elif [[ ${rc} -eq 124 ]]; then
    echo "TIMEOUT after ${NUMA_TIMEOUT_SECONDS}s; results may be incomplete" > "${stem}.status"
  else
    echo "FAILED rc=${rc}; results may be incomplete" > "${stem}.status"
  fi
  return "${rc}"
}

summarize_dir() {
  local out_dir="$1"
  python3 "${SCRIPT_DIR}/numa_summarize.py" "${out_dir}" | tee "${out_dir}/summary.md"
}
