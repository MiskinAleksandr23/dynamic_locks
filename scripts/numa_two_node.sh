#!/usr/bin/env bash

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THREADS="${THREADS:-64}"
NODES="${NODES:-0,1}"
source "${SCRIPT_DIR}/numa_common.sh"

OUT_DIR="$(make_result_dir "nodes${NODES//,/}_${THREADS}t")"
write_topology "${OUT_DIR}"
rc=0
run_suite "${OUT_DIR}" "nodes${NODES//,/}" "${THREADS}" \
  --cpunodebind="${NODES}" --interleave="${NODES}" || rc=$?
summarize_dir "${OUT_DIR}"
echo "Results: ${OUT_DIR}"
exit "${rc}"
