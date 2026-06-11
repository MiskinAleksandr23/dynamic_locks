#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BENCH_BIN="${REPO_ROOT}/build/adaptive_lock_benchmark"
RESULT_BASE="${RESULT_BASE:-${REPO_ROOT}/BENCHMARK_RESULT/numa}"
NUMA_PROFILE="${NUMA_PROFILE:-balanced}"
NUMA_TIMEOUT_SECONDS="${NUMA_TIMEOUT_SECONDS:-300}"

COMMON_ENV=(
  DYNAMIC_LOCK_RUN_GENETIC=1
  DYNAMIC_LOCK_REBUILD_INTERVAL_MS="${DYNAMIC_LOCK_REBUILD_INTERVAL_MS:-10000}"
  DYNAMIC_LOCK_REBUILD_THRESHOLD="${DYNAMIC_LOCK_REBUILD_THRESHOLD:-2.0}"
  DYNAMIC_LOCK_STATS_SAMPLE_RATE="${DYNAMIC_LOCK_STATS_SAMPLE_RATE:-64}"
  DYNAMIC_LOCK_REBUILD_MIN_GAIN="${DYNAMIC_LOCK_REBUILD_MIN_GAIN:-1.0}"
  DYNAMIC_LOCK_REBUILD_MIN_SKEW="${DYNAMIC_LOCK_REBUILD_MIN_SKEW:-4.0}"
  DYNAMIC_LOCK_GENETIC_TRAINING_BATCH="${DYNAMIC_LOCK_GENETIC_TRAINING_BATCH:-100000}"
  DYNAMIC_LOCK_GENETIC_TRAINING_SAMPLE_RATE="${DYNAMIC_LOCK_GENETIC_TRAINING_SAMPLE_RATE:-64}"
  DYNAMIC_LOCK_GENETIC_PROBE_GAP="${DYNAMIC_LOCK_GENETIC_PROBE_GAP:-1000000}"
  DYNAMIC_LOCK_GENETIC_MIN_SKEW="${DYNAMIC_LOCK_GENETIC_MIN_SKEW:-4.0}"
)

require_benchmark() {
  if [[ ! -x "${BENCH_BIN}" ]]; then
    echo "Missing ${BENCH_BIN}. Run scripts/numa_build.sh first." >&2
    exit 1
  fi
}

make_result_dir() {
  local label="$1"
  local stamp
  stamp="$(date -u +%Y%m%d_%H%M%S)"
  local dir="${RESULT_BASE}/${stamp}_${label}_${NUMA_PROFILE}"
  mkdir -p "${dir}"
  printf '%s\n' "${dir}"
}

write_topology() {
  local out_dir="$1"
  {
    echo "# date -u"
    date -u
    echo
    echo "# uname -a"
    uname -a
    echo
    echo "# lscpu"
    lscpu || true
    echo
    echo "# numactl --hardware"
    if command -v numactl >/dev/null 2>&1; then
      numactl --hardware || true
    else
      echo "numactl not found"
    fi
  } > "${out_dir}/topology.txt"
}

scenario_divisor() {
  local scenario="$1"
  case "${NUMA_PROFILE}" in
    smoke)
      case "${scenario}" in
        moving_small_window) echo 120 ;;
        *) echo 80 ;;
      esac
      ;;
    quick)
      case "${scenario}" in
        moving_small_window) echo 80 ;;
        *) echo 40 ;;
      esac
      ;;
    balanced)
      case "${scenario}" in
        moving_small_window) echo 60 ;;
        *) echo 30 ;;
      esac
      ;;
    final)
      case "${scenario}" in
        moving_small_window) echo 40 ;;
        *) echo 20 ;;
      esac
      ;;
    *)
      echo "Unknown NUMA_PROFILE=${NUMA_PROFILE}; use smoke, quick, balanced, or final." >&2
      exit 1
      ;;
  esac
}

estimate_for() {
  local scenario="$1"
  case "${NUMA_PROFILE}:${scenario}" in
    smoke:*) echo "expected roughly < 1 min" ;;
    quick:moving_small_window) echo "expected roughly 2-4 min" ;;
    quick:*) echo "expected roughly 1-3 min" ;;
    balanced:moving_small_window) echo "expected roughly 3-5 min" ;;
    balanced:*) echo "expected roughly 2-4 min" ;;
    final:moving_small_window) echo "may reach the ${NUMA_TIMEOUT_SECONDS}s timeout" ;;
    final:*) echo "expected roughly 3-5 min" ;;
    *) echo "rough estimate unavailable" ;;
  esac
}

run_one_scenario() {
  local out_dir="$1"
  local placement_label="$2"
  local threads="$3"
  local scenario="$4"
  local divisor="${5:-$(scenario_divisor "${scenario}")}"
  shift 5 || true
  local numa_args=("$@")

  require_benchmark

  local output="${out_dir}/${placement_label}_${threads}t_${scenario}_div${divisor}.txt"
  local status_file="${output%.txt}.status"
  local estimate
  estimate="$(estimate_for "${scenario}")"

  echo
  echo "=== ${placement_label} ${threads} threads ${scenario} divisor=${divisor} ==="
  echo "${estimate}; timeout=${NUMA_TIMEOUT_SECONDS}s"
  echo "output=${output}"

  local cmd=(
    env
    "${COMMON_ENV[@]}"
    DYNAMIC_LOCK_THREAD_COUNT="${threads}"
    DYNAMIC_LOCK_QUERY_DIVISOR="${divisor}"
    DYNAMIC_LOCK_SCENARIO="${scenario}"
  )

  if ((${#numa_args[@]} > 0)); then
    if ! command -v numactl >/dev/null 2>&1; then
      echo "numactl is required for this run but was not found." >&2
      return 127
    fi
    cmd+=(numactl "${numa_args[@]}")
  fi

  cmd+=("${BENCH_BIN}")

  {
    echo "# command"
    printf '%q ' "${cmd[@]}"
    echo
    echo "# profile=${NUMA_PROFILE} timeout=${NUMA_TIMEOUT_SECONDS}s estimate=${estimate}"
    echo
  } > "${output}"

  set +e
  timeout "${NUMA_TIMEOUT_SECONDS}" "${cmd[@]}" 2>&1 | tee -a "${output}"
  local rc=${PIPESTATUS[0]}
  set -e

  if [[ ${rc} -eq 0 ]]; then
    echo "OK" > "${status_file}"
  elif [[ ${rc} -eq 124 ]]; then
    echo "TIMEOUT after ${NUMA_TIMEOUT_SECONDS}s" > "${status_file}"
    echo "TIMEOUT: ${scenario} exceeded ${NUMA_TIMEOUT_SECONDS}s" >&2
  else
    echo "FAILED rc=${rc}" > "${status_file}"
    echo "FAILED: ${scenario} rc=${rc}" >&2
  fi

  return "${rc}"
}

summarize_dir() {
  local out_dir="$1"
  "${SCRIPT_DIR}/numa_summarize.py" "${out_dir}" | tee "${out_dir}/summary.md"
}
