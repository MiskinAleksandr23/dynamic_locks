#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THREADS="${THREADS:-16}"
NODE="${NODE:-0}"
NUMA_PROFILE="${NUMA_PROFILE:-smoke}"
NUMA_TIMEOUT_SECONDS="${NUMA_TIMEOUT_SECONDS:-180}"

source "${SCRIPT_DIR}/numa_common.sh"

OUT_DIR="$(make_result_dir "smoke_node${NODE}_${THREADS}t")"
write_topology "${OUT_DIR}"

run_one_scenario "${OUT_DIR}" "node${NODE}" "${THREADS}" \
  "clustered_4_thread_groups" "$(scenario_divisor clustered_4_thread_groups)" \
  --cpunodebind="${NODE}" --membind="${NODE}"

summarize_dir "${OUT_DIR}"
echo "Results: ${OUT_DIR}"
