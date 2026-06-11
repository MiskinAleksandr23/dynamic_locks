#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THREADS="${THREADS:-128}"
NUMA_PROFILE="${NUMA_PROFILE:-quick}"
NUMA_TIMEOUT_SECONDS="${NUMA_TIMEOUT_SECONDS:-300}"

source "${SCRIPT_DIR}/numa_common.sh"

SCENARIOS=(
  clustered_4_thread_groups
  clustered_4_thread_groups_contended
)

OUT_DIR="$(make_result_dir "allnodes_${THREADS}t")"
write_topology "${OUT_DIR}"

for scenario in "${SCENARIOS[@]}"; do
  run_one_scenario "${OUT_DIR}" "allnodes" "${THREADS}" \
    "${scenario}" "$(scenario_divisor "${scenario}")" \
    --interleave=all || true
done

summarize_dir "${OUT_DIR}"
echo "Results: ${OUT_DIR}"
