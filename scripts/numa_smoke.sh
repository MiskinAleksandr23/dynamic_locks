#!/usr/bin/env bash

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export THREADS="${THREADS:-16}"
export OPERATIONS="${OPERATIONS:-100000}"
export REPETITIONS="${REPETITIONS:-1}"
export NUMA_TIMEOUT_SECONDS="${NUMA_TIMEOUT_SECONDS:-180}"
exec "${SCRIPT_DIR}/numa_one_node.sh"
