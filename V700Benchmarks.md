# V700 Adaptive Lock Benchmarks

Date: 2026-06-02 UTC

Host: `x86_64`, Linux `6.8.0-117-generic`, 8 vCPUs, `AMD EPYC Processor` under KVM/QEMU.

Build: clang++ 18.1.3, `CMAKE_BUILD_TYPE=Release`, `BUILD_BENCHMARK_V700=ON`, `PAPI=OFF`, `OPENMP=OFF`.

Raw outputs:

- `BENCHMARK_RESULT/v700_raw/online/`

## Build And Run

```bash
cmake -S . -B build \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_BENCHMARK_V700=ON \
  -DPAPI=OFF \
  -DOPENMP=OFF

cmake --build build --target \
  adaptive_lock_naive.debra \
  adaptive_lock_dynamic.debra \
  adaptive_lock_genetic.debra \
  -j
```

Example run:

```bash
./build/Benchmark-v700-build/adaptive_lock_dynamic.debra \
  -json-file Benchmark-v700/cpp/microbench/json_example/adaptive_lock_v700_warmup_hotspot_point.json
```

The `.debra` suffix is the V700 reclaimer configuration name. Our adaptive locks do not use V700 memory reclamation internally, but the framework builds all data structures through this target naming scheme.

## Method

These are online V700 runs. The current workloads have `prefill.numThreads=0` and `warmUp.numThreads=0`; all reported throughput is from the V700 `test` stage.

`Benchmark-v700/cpp/ds/adaptive_lock/adapter_impl.h::warmupEnd()` is intentionally a no-op for these adapters, so online rebuild/training remains enabled during the measured stage. `Benchmark-v700/cpp/microbench/main.cpp` calls an optional `testEnd()` hook after the test workers finish and recomputes `elapsedMillis`, so rebuilder/training stop and join time are also included in the V700 elapsed time and throughput.

The adapter destructor only repeats an idempotent stop. It does not flush pending genetic samples or force a rebuild after the measured stage, so there is no useful adaptation work hidden outside the V700 timer.

Adapter parameters:

- `range`: `1,048,576`
- `test.numThreads`: `8`
- locks: `64`
- fine blocks: `1024`
- dynamic: stats sample rate `64`, rebuild interval `500ms`, threshold `2.0`, minimum skew `4.0`
- genetic: stats sample rate `64`, training batch `100,000`, history `12,000`, probe gap `1,000,000`, deterministic partitioner mode with `generation_count=0`

## Workloads

| Workload | V700 JSON | Shape |
|---|---|---|
| Online hotspot point | `Benchmark-v700/cpp/microbench/json_example/adaptive_lock_v700_warmup_hotspot_point.json` | 60M point reads, 99% traffic in a 16,384-key hot window |
| Online hotspot short range | `Benchmark-v700/cpp/microbench/json_example/adaptive_lock_v700_warmup_hotspot_range.json` | 60M range queries, `interval=8`, 99% traffic in a 16,384-key hot window |
| Online moving-window short range | `Benchmark-v700/cpp/microbench/json_example/adaptive_lock_v700_moving_window_range.json` | 112M range queries, `interval=8`, 16,384-key window, 14 positions, 8M operations per position |

The point workload uses V700 `GET_FUNC` through `find`. The range workloads use V700 `rangeQuery`; the adapter intentionally performs a small write inside the locked range so the lock path is exercised.

## Results

Single-run Release snapshot. `vs naive` is `query throughput` divided by the naive throughput for the same workload. All rows reported `Structural validation OK`.

| Workload | Implementation | Query throughput ops/s | vs naive | Total ops | Avg lock ms | Reconfigs/train | Wall s |
|---|---|---:|---:|---:|---:|---:|---:|
| Online hotspot point | naive | 1,531,901 | 1.000x | 60,000,000 | 0.003570 | 0 | 39.167 |
| Online hotspot point | dynamic | 4,187,604 | 2.734x | 60,000,000 | 0.001112 | 3 | 14.328 |
| Online hotspot point | genetic | 2,875,904 | 1.877x | 60,000,000 | 0.001554 | 1 | 20.863 |
| Online hotspot short range | naive | 951,022 | 1.000x | 60,000,000 | 0.005503 | 0 | 63.090 |
| Online hotspot short range | dynamic | 1,553,116 | 1.633x | 60,000,000 | 0.003052 | 3 | 38.632 |
| Online hotspot short range | genetic | 1,742,210 | 1.832x | 60,000,000 | 0.002613 | 1 | 34.439 |
| Online moving-window short range | naive | 1,011,204 | 1.000x | 112,000,000 | 0.004757 | 0 | 110.759 |
| Online moving-window short range | dynamic | 1,790,395 | 1.771x | 112,000,000 | 0.002369 | 9 | 62.556 |
| Online moving-window short range | genetic | 1,505,153 | 1.488x | 112,000,000 | 0.002839 | 2 | 74.411 |

## Summary

| Workload | Dynamic vs naive | Genetic vs naive | Best result |
|---|---:|---:|---|
| Online hotspot point | 2.734x | 1.877x | dynamic |
| Online hotspot short range | 1.633x | 1.832x | genetic |
| Online moving-window short range | 1.771x | 1.488x | dynamic |
