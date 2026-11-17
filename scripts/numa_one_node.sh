#!/usr/bin/env bash

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THREADS="${THREADS:-32}"
NODE="${NODE:-0}"
source "${SCRIPT_DIR}/numa_common.sh"

OUT_DIR="$(make_result_dir "node${NODE}_${THREADS}t")"
write_topology "${OUT_DIR}"
rc=0
run_suite "${OUT_DIR}" "node${NODE}" "${THREADS}" \
  --cpunodebind="${NODE}" --membind="${NODE}" || rc=$?
summarize_dir "${OUT_DIR}"
echo "Results: ${OUT_DIR}"
exit "${rc}"
