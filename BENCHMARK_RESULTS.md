# Dynamic Lock Benchmark Results

Snapshot from `./cmake-build-release/adaptive_lock_benchmark`.

`Benchmark-v700` is vendored, but the top-level build does not build it unless
`-DBUILD_BENCHMARK_V700=ON` is passed.

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
| Small hot window size | 1,024 |
| Small range max length | 8 |
| Moving window size | 4,096 |
| Moving window stops | 14 |
| Main lock primitive | `std::mutex` |

For a slow server run of the moving-window scenario, set:

```bash
DYNAMIC_LOCK_MOVING_SECONDS_PER_DIRECTION=300 ./cmake-build-release/adaptive_lock_benchmark
```

That keeps each moving-window direction slow enough for online repartitioning.
The local snapshot below uses the faster default fixed-query adaptation.

## Column Meaning

| Column | Meaning |
|---|---|
| Setup time | Warmup/adaptation time before measured requests |
| Measured request time | Final measured phase with real body: lock, sum range, update one element, unlock |
| Request speedup | Speedup over naive fixed partitioning in the same scenario |
| Empty critical section time | Same coordinates, but empty body inside the lock |
| Empty-body speedup | Speedup over naive fixed partitioning for empty critical section |
| Avg lock wait | Average measured time from entering `WriteQuery` to locks acquired |
| Total lock wait | Sum of measured lock acquisition time across workers |
| Avg mutexes/query | Average number of mutexes acquired per measured query |
| Hot locks L/R | Number of mutexes covering left/right 16,384-element hot window after setup |
| Rebuild/train | Dynamic rebuilds or genetic training batches |

## Scenario: Shifted Hotspot, Point Queries

Right-side warmup, then final point requests in the left hot window.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.054 | 0.245 | 1.000x | 0.177 | 1.000x | 1.42 | 1.708 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.365 | 0.161 | 1.521x | 0.155 | 1.141x | 1.04 | 1.251 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.708 | 0.149 | 1.641x | 0.142 | 1.246x | 0.95 | 1.144 | 1.00 | 16 / 1 | 7 |

## Scenario: Clustered Small Hot Windows

Four 1,024-element hot windows inside one coarse naive partition. Requests are
small random ranges of length `1..8`.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.052 | 0.251 | 1.000x | 0.179 | 1.000x | 1.45 | 1.742 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.374 | 0.214 | 1.169x | 0.184 | 0.974x | 1.41 | 1.686 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.294 | 0.199 | 1.259x | 0.167 | 1.076x | 1.31 | 1.569 | 1.00 | 12 / 1 | 7 |

Result: both adaptive variants benefit from splitting one overloaded naive
partition into several hot sub-partitions. Empty-body speedups are smaller
because the request body is intentionally tiny.

## Scenario: Moving Small Window

A 4,096-element window moves left-to-right and back. For each stop, the
benchmark adapts/rebuilds first, then measures with online adaptation disabled.
This checks whether the learned layout is useful after the window has moved
slowly enough to converge.

Setup queries: `128,000 + 2,688,000 per-stop adapt`. Measured queries:
`9,800,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.575 | 2.003 | 1.000x | 1.445 | 1.000x | 1.41 | 13.851 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 1.386 | 1.690 | 1.186x | 1.392 | 1.038x | 1.35 | 13.183 | 1.00 | 16 / 1 | 14 |
| Genetic | 4.228 | 1.729 | 1.159x | 1.364 | 1.060x | 1.34 | 13.149 | 1.00 | 1 / 1 | 66 |

Result: after fixing runtime lookup to O(1), the moving-window case shows a
measured win for both adaptive implementations. The setup cost is intentionally
higher because every window stop gets its own adaptation phase.

## Scenario: Uniform Random, Point Queries

Uniform random point updates over the whole array.

Setup queries: `64,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.007 | 0.129 | 1.000x | 0.127 | 1.000x | 0.69 | 0.824 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.011 | 0.146 | 0.883x | 0.146 | 0.871x | 0.76 | 0.909 | 1.00 | 1 / 1 | 0 |
| Genetic | 0.425 | 0.141 | 0.915x | 0.138 | 0.921x | 0.72 | 0.868 | 1.00 | 1 / 1 | 3 |

## Scenario: Shifted Hotspot, Random-Length Ranges

Right-side warmup, then left hot-window random-length ranges.

Setup queries: `16,000 + 48,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.089 | 0.058 | 1.000x | 0.006 | 1.000x | 11.52 | 0.461 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.422 | 0.077 | 0.762x | 0.011 | 0.509x | 15.49 | 0.620 | 1.00 | 1 / 1 | 1 |
| Genetic | 0.649 | 0.046 | 1.281x | 0.014 | 0.404x | 11.08 | 0.443 | 1.23 | 2 / 1 | 4 |

## Scenario: Uniform Random, Random-Length Ranges

Uniform random ranges over the whole array.

Setup queries: `16,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.016 | 0.038 | 1.000x | 0.011 | 1.000x | 7.50 | 0.300 | 2.99 | 1 / 1 | 0 |
| Dynamic DP | 0.015 | 0.041 | 0.912x | 0.016 | 0.697x | 8.21 | 0.328 | 2.99 | 1 / 1 | 0 |
| Genetic | 0.398 | 0.039 | 0.958x | 0.012 | 0.938x | 7.88 | 0.315 | 3.03 | 1 / 1 | 2 |

## Implementation Notes

The moving-window case exposed a real runtime issue: both adaptive lock
implementations used a linear scan through partition cuts to map a block to a
mutex. That was cheap for left-side hotspots, but expensive when the hot window
moved across the whole array. The lock implementations now maintain an O(1)
`block -> mutex` lookup table and rebuild it whenever partitions change.

## Takeaways

| Case | Best final measured time | Comment |
|---|---|---|
| Shifted hotspot, point queries | Genetic, 1.641x over naive | Both adaptive variants split the hot region into 16 locks |
| Clustered small hot windows | Genetic, 1.259x over naive | Matches the “several hot intervals” idea from the paper |
| Moving small window | Dynamic DP, 1.186x over naive | Slow per-stop adaptation lets the window repartition before measurement |
| Uniform random, point queries | Naive | No stable locality to exploit |
| Shifted hotspot, random ranges | Genetic, 1.281x over naive | Dynamic keeps ranges coarse; genetic is faster on body time here |
| Uniform random, random ranges | Naive | Multi-mutex requests exist, but no exploitable structure |
