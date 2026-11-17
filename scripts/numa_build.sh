#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cmake -S "${REPO_ROOT}" -B "${REPO_ROOT}/build" \
  -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSANITIZER= \
  -DBUILD_GENETIC_BENCHMARK=OFF \
  -DBUILD_TESTING=ON \
  -DBUILD_BENCHMARK_V700=OFF

cmake --build "${REPO_ROOT}/build" \
  -j "${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
ctest --test-dir "${REPO_ROOT}/build" --output-on-failure
