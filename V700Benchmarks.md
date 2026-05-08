# V700 Adaptive Lock Benchmarks

Date: 2026-05-08 UTC

Repository revision: `0b99708` plus local V700 adapter/config fixes.

Host: `x86_64`, Linux `6.8.0-107-generic`, 8 vCPUs, `AMD EPYC Processor` under QEMU.

## Scope

The top-level `benchmarks/` directory was not used for these numbers. The runs
use only `Benchmark-v700` binaries:

- `Benchmark-v700/cpp/build/adaptive_lock_naive.debra`
- `Benchmark-v700/cpp/build/adaptive_lock_dynamic.debra`
- `Benchmark-v700/cpp/build/adaptive_lock_genetic.debra`

Raw outputs are under:

- `BENCHMARK_RESULT/v700_raw/diagnostic_warmup/`

## Method

These V700 workloads are shaped to match the useful mode from the local
adaptive-lock benchmark:

1. Run a warmup/adaptation phase.
2. Force pending dynamic/genetic adaptation in `warmupEnd()`.
3. Stop online rebuilding/training and reset runtime stats.
4. Measure the final test phase only.

The measured workloads use many short hotspot queries, because this is the case
where adaptive partitioning is supposed to reduce contention. Long random ranges
or uniform traffic are not good target scenarios for this lock design.

Relevant V700 integration changes:

- `Benchmark-v700/cpp/microbench/main.cpp` calls `g->dsAdapter->warmupEnd()` after warmup.
- `Benchmark-v700/cpp/ds/adaptive_lock/adapter_impl.h` forces adaptation, stops rebuilder/training, and resets runtime stats in `warmupEnd()`.
- The adapter implements point `insert`, `insertIfAbsent`, `erase`, `find`, and `contains`, so V700 prefill and point workloads work correctly.

## Workloads

| Workload | V700 JSON | Shape |
|---|---|---|
| Warmup hotspot point | `Benchmark-v700/cpp/microbench/json_example/adaptive_lock_v700_warmup_hotspot_point.json` | 10M warmup point queries, 20M measured point queries, 99% traffic in a 16,384-key hot window |
| Warmup hotspot short range | `Benchmark-v700/cpp/microbench/json_example/adaptive_lock_v700_warmup_hotspot_range.json` | 10M warmup range queries, 20M measured range queries, short `interval=8`, 99% traffic in a 16,384-key hot window |

## V700 Parameters

Common parameters:

- `range`: `1,048,576`
- `test.numThreads`: `8`
- `prefill.numThreads`: `0`
- `warmUp.stopCondition`: `OperationCounter`, `commonOperationLimit=10,000,000`
- `test.stopCondition`: `OperationCounter`, `commonOperationLimit=20,000,000`
- `threadLoopBuilder`: `DefaultThreadLoopBuilder`
- `dataMapBuilder`: `IdDataMapBuilder`

Point hotspot workload:

- `argsGeneratorBuilder`: `TemporarySkewedArgsGeneratorBuilder`
- `hotSize`: `0.015625`, i.e. 16,384 keys out of 1,048,576
- `hotRatio`: `0.99`
- `setBegins`: `[0.0]`
- operation mix: `insertRatio=0.0`, `removeRatio=0.0`, `rqRatio=0.0`, so all measured operations are point reads through `GET_FUNC`

Short range hotspot workload:

- `argsGeneratorBuilder`: `RangeQueryArgsGeneratorBuilder`
- `distributionBuilder`: `SkewedUniformDistributionBuilder`
- `hotSize`: `0.015625`
- `hotRatio`: `0.99`
- `interval`: `8`
- operation mix: `insertRatio=0.0`, `removeRatio=0.0`, `rqRatio=1.0`, so all measured operations are range queries

## Results

Single-run snapshot. `vs naive` is throughput divided by the `naive`
throughput for the same workload.

| Workload | Implementation | Throughput ops/s | vs naive | Total ops | Avg lock ms | Reconfigs | Wall s |
|---|---|---:|---:|---:|---:|---:|---:|
| Warmup hotspot point | naive | 1,660,026 | 1.000x | 20,000,000 | 0.003313 | 0 | 18.859 |
| Warmup hotspot point | dynamic | 3,942,440 | 2.375x | 20,000,000 | 0.001170 | 1 | 12.113 |
| Warmup hotspot point | genetic | 3,890,293 | 2.344x | 20,000,000 | 0.001059 | 42 | 12.517 |
| Warmup hotspot short range | naive | 1,145,672 | 1.000x | 20,000,000 | 0.004531 | 0 | 26.833 |
| Warmup hotspot short range | dynamic | 3,229,452 | 2.819x | 20,000,000 | 0.001150 | 1 | 17.339 |
| Warmup hotspot short range | genetic | 3,471,017 | 3.030x | 20,000,000 | 0.001057 | 76 | 19.042 |

## Summary

| Workload | Dynamic vs naive | Genetic vs naive | Best result |
|---|---:|---:|---|
| Warmup hotspot point | 2.375x | 2.344x | dynamic |
| Warmup hotspot short range | 2.819x | 3.030x | genetic |

## Notes

The reported numbers use the warmup/adapt phase only to learn the partitioning.
Throughput is measured after adaptation is forced, online rebuilding/training is
stopped, and runtime counters are reset.
