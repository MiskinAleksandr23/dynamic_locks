#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

cmake -S . -B build \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BENCHMARK_V700=OFF

cmake --build build --target adaptive_lock_benchmark genetic_lock_benchmark -j "${JOBS:-$(nproc)}"
