#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THREADS="${THREADS:-64}"
NODES="${NODES:-0,1}"
NUMA_PROFILE="${NUMA_PROFILE:-balanced}"
NUMA_TIMEOUT_SECONDS="${NUMA_TIMEOUT_SECONDS:-300}"

source "${SCRIPT_DIR}/numa_common.sh"

SCENARIOS=(
  clustered_4_thread_groups
  clustered_4_thread_groups_contended
  moving_small_window
)

OUT_DIR="$(make_result_dir "nodes${NODES//,/}_${THREADS}t")"
write_topology "${OUT_DIR}"

for scenario in "${SCENARIOS[@]}"; do
  run_one_scenario "${OUT_DIR}" "nodes${NODES//,/}" "${THREADS}" \
    "${scenario}" "$(scenario_divisor "${scenario}")" \
    --cpunodebind="${NODES}" --interleave="${NODES}" || true
done

summarize_dir "${OUT_DIR}"
echo "Results: ${OUT_DIR}"
