# Dynamic Lock Benchmark Results

Snapshot from `./cmake-build-release/benchmark`.

## Configuration

| Parameter | Value |
|---|---:|
| Array size | 1,048,576 |
| Mutexes | 64 |
| Fine blocks | 1,024 |
| Threads | 11 |
| Point query length | 1 |
| Random range max length | 65,536 |
| Hot window size | 16,384 |

## Column Meaning

| Column | Meaning |
|---|---|
| Setup time | Time spent on warmup/adaptation phases before final measurement |
| Measured request time | Final phase with real request body: lock, read/sum range, update one element, unlock |
| Request speedup | Speedup over naive fixed partitioning in the same scenario |
| Empty critical section time | Final phase with the same request coordinates, but empty body inside the lock |
| Empty-body speedup | Speedup over naive fixed partitioning for empty critical section |
| Avg lock wait | Average measured time from entering `WriteQuery` to locks acquired |
| Total lock wait | Sum of measured lock acquisition time across all worker threads |
| Avg mutexes/query | Average number of mutexes acquired by one measured query |
| Hot locks L/R | Number of mutexes covering left/right hot window after setup |
| Rebuild/train | Number of dynamic rebuilds or genetic training batches |

## Scenario: Shifted Hotspot, Point Queries

Right-side warmup, then workload shifts to the left hot window. Final measured requests are point updates inside the left hot window.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.058 | 0.263 | 1.000x | 0.176 | 1.000x | 1.52 | 1.824 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.375 | 0.164 | 1.600x | 0.159 | 1.111x | 1.05 | 1.265 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.732 | 0.143 | 1.834x | 0.133 | 1.324x | 0.93 | 1.117 | 1.00 | 16 / 1 | 7 |

Result: both adaptive implementations split the left hot window across 16 mutexes. This is beneficial for point requests because each measured query still acquires one mutex, but contention is spread across the hot window. Genetic has higher setup cost, but the fastest final phase after convergence.

## Scenario: Uniform Random, Point Queries

Warmup and final measured requests are uniformly random point updates over the whole array.

Setup queries: `64,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.007 | 0.134 | 1.000x | 0.132 | 1.000x | 0.70 | 0.837 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.011 | 0.152 | 0.879x | 0.153 | 0.864x | 0.78 | 0.932 | 1.00 | 1 / 1 | 0 |
| Genetic | 0.397 | 0.142 | 0.943x | 0.149 | 0.891x | 0.74 | 0.890 | 1.00 | 1 / 2 | 3 |

Result: adaptive partitioning does not help on uniform random point requests. There is no stable hotspot to exploit, so the extra adaptation/runtime overhead dominates.

## Scenario: Shifted Hotspot, Random-Length Ranges

Right-side warmup, then workload shifts to the left hot window. Final measured requests are ranges inside the left hot window. Range length is random; because the hot window is `16,384` elements, lengths are effectively capped by that window even though the global max is `65,536`.

Setup queries: `16,000 + 48,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.086 | 0.056 | 1.000x | 0.006 | 1.000x | 10.90 | 0.436 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.411 | 0.074 | 0.751x | 0.014 | 0.408x | 14.60 | 0.584 | 1.00 | 1 / 1 | 1 |
| Genetic | 0.650 | 0.045 | 1.242x | 0.014 | 0.412x | 10.82 | 0.433 | 1.23 | 2 / 1 | 4 |

Result: this case shows the range-query tradeoff. Naive and Dynamic DP keep the left hot window coarse, so measured range requests acquire one mutex on average. Genetic uses a slightly finer split (`2 / 1` hot locks), which raises average mutexes per query to `1.23`, but it still has the fastest measured request time in this run. For empty critical sections, all adaptive variants remain slower than naive because setup/adaptation and multi-lock overhead are not offset by useful work.

## Scenario: Uniform Random, Random-Length Ranges

Warmup and final measured requests are uniformly random ranges over the whole array with random length from `1` to `65,536`.

Setup queries: `16,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.016 | 0.037 | 1.000x | 0.011 | 1.000x | 7.37 | 0.295 | 2.99 | 1 / 1 | 0 |
| Dynamic DP | 0.016 | 0.040 | 0.925x | 0.017 | 0.643x | 8.01 | 0.320 | 2.99 | 1 / 1 | 0 |
| Genetic | 0.393 | 0.039 | 0.935x | 0.013 | 0.870x | 7.94 | 0.318 | 3.03 | 1 / 1 | 2 |

Result: this scenario explicitly exercises multi-mutex requests. All implementations acquire about `3` mutexes per query on average. Since the distribution is uniform, adaptive partitioning has no stable hotspot to exploit; differences are mostly runtime overhead and benchmark noise.

## Takeaways

| Case | Best final measured time | Comment |
|---|---|---|
| Shifted hotspot, point queries | Genetic, 1.834x over naive | Hot window is split into 16 locks and each point query still takes one mutex |
| Uniform random, point queries | Naive | No concentrated hotspot |
| Shifted hotspot, random-length ranges | Genetic, 1.242x over naive | Range-aware DP keeps the hot window coarse; genetic is still fastest on body time in this run |
| Uniform random, random-length ranges | Naive | Multi-mutex requests are visible, but no adaptive strategy has structure to exploit |
