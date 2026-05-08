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
| Clustered hot window size | 16,384 |
| Clustered churn changes | 5 |
| Small range max length | 8 |
| Moving window size | 4,096 |
| Moving window stops | 14 |
| Main lock primitive | `std::mutex` |

For a slow run of the moving-window scenario, set:

```bash
DYNAMIC_LOCK_MOVING_SECONDS_PER_DIRECTION=60 ./cmake-build-release/adaptive_lock_benchmark
```

That keeps each moving-window direction slow enough for online repartitioning.
The local snapshot below uses the faster default fixed-query adaptation, without
this environment variable.

To run only one group of scenarios, set `DYNAMIC_LOCK_SCENARIO` to a substring
of the scenario name. For example:

```bash
DYNAMIC_LOCK_SCENARIO=clustered_churn ./cmake-build-release/adaptive_lock_benchmark
```

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

Raw benchmark output names: `body_s` is "Measured request time",
`body_x` is "Request speedup", `lock_only_s` is "Empty critical section time",
and `lock_x` is "Empty-body speedup".

## Scenario: Shifted Hotspot, Point Queries

Right-side warmup, then final point requests in the left hot window.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.046 | 0.212 | 1.000x | 0.139 | 1.000x | 1.23 | 1.478 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.361 | 0.155 | 1.370x | 0.144 | 0.965x | 0.96 | 1.158 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.705 | 0.139 | 1.523x | 0.131 | 1.059x | 0.90 | 1.076 | 1.00 | 16 / 1 | 7 |

## Scenario: Clustered 2 Hot Windows

Two compact 16,384-element hot windows. Requests are point updates. The hot
windows start at fine blocks `16` and `48`, so there is a cold gap before,
between, and after the hot intervals.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.044 | 0.210 | 1.000x | 0.152 | 1.000x | 1.29 | 1.547 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.363 | 0.131 | 1.605x | 0.126 | 1.201x | 0.80 | 0.959 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.750 | 0.127 | 1.655x | 0.124 | 1.223x | 0.77 | 0.927 | 1.00 | 1 / 1 | 7 |

## Scenario: Clustered 4 Hot Windows

Four compact 16,384-element hot windows. Requests are point updates. The hot
windows start at fine blocks `16`, `48`, `80`, and `112`, giving the intended
cold/hot/cold/hot/cold/hot/cold/hot/cold structure.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.035 | 0.181 | 1.000x | 0.149 | 1.000x | 1.17 | 1.406 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.359 | 0.144 | 1.253x | 0.140 | 1.066x | 0.78 | 0.931 | 1.00 | 1 / 1 | 1 |
| Genetic | 1.018 | 0.129 | 1.402x | 0.129 | 1.150x | 0.72 | 0.870 | 1.00 | 1 / 1 | 7 |

Result: the old 1,024-element clustered windows were exactly one fine block, so
adaptive implementations could not split inside a window. With 16,384-element
windows, each hot interval contains 16 fine blocks. The `Hot locks L/R` column
is not meaningful for compact clustered scenarios because it still reports the
fixed left/right 16,384-element diagnostic windows, not the cluster windows.

## Scenario: Clustered Churn, 2 Hot Windows

Two compact 16,384-element hot windows. There are 5 timed phases; in every
phase, half of the active windows is replaced by new compact windows before
measuring point requests.

Setup queries: `64,000 + 480,000 timed adapt`. Measured queries: `1,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.083 | 0.155 | 1.000x | 0.125 | 1.000x | 1.15 | 1.154 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.585 | 0.114 | 1.359x | 0.113 | 1.106x | 0.83 | 0.835 | 1.00 | 16 / 1 | 5 |
| Genetic | 1.871 | 0.109 | 1.425x | 0.106 | 1.186x | 0.81 | 0.805 | 1.00 | 1 / 1 | 18 |

## Scenario: Clustered Churn, 4 Hot Windows

Four compact 16,384-element hot windows. There are 5 timed phases; in every
phase, half of the active windows is replaced by new compact windows before
measuring point requests.

Setup queries: `64,000 + 480,000 timed adapt`. Measured queries: `1,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.072 | 0.133 | 1.000x | 0.117 | 1.000x | 1.02 | 1.025 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.585 | 0.117 | 1.131x | 0.118 | 0.993x | 0.73 | 0.732 | 1.00 | 1 / 1 | 5 |
| Genetic | 2.330 | 0.110 | 1.212x | 0.105 | 1.117x | 0.72 | 0.720 | 1.00 | 1 / 1 | 18 |

## Scenario: Moving Small Window

A 4,096-element window moves left-to-right and back. For each stop, the
benchmark adapts/rebuilds first, then measures with online adaptation disabled.
This checks whether the learned layout is useful after the window has moved
slowly enough to converge.

Setup queries: `128,000 + 2,688,000 per-stop adapt`. Measured queries:
`9,800,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.520 | 1.835 | 1.000x | 1.115 | 1.000x | 1.35 | 13.228 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 1.295 | 1.550 | 1.184x | 1.221 | 0.913x | 1.23 | 12.016 | 1.00 | 16 / 1 | 14 |
| Genetic | 4.145 | 1.545 | 1.188x | 1.214 | 0.918x | 1.22 | 11.912 | 1.00 | 1 / 1 | 68 |

Result: after fixing runtime lookup to O(1), the moving-window case shows a
measured win for both adaptive implementations. The setup cost is intentionally
higher because every window stop gets its own adaptation phase.

## Scenario: Uniform Random, Point Queries

Uniform random point updates over the whole array.

Setup queries: `64,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.006 | 0.119 | 1.000x | 0.121 | 1.000x | 0.64 | 0.771 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.011 | 0.122 | 0.975x | 0.129 | 0.935x | 0.66 | 0.792 | 1.00 | 1 / 1 | 0 |
| Genetic | 0.419 | 0.138 | 0.864x | 0.132 | 0.918x | 0.72 | 0.869 | 1.00 | 1 / 2 | 3 |

## Scenario: Shifted Hotspot, Random-Length Ranges

Right-side warmup, then left hot-window random-length ranges.

Setup queries: `16,000 + 48,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.085 | 0.053 | 1.000x | 0.005 | 1.000x | 10.49 | 0.420 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.406 | 0.080 | 0.667x | 0.018 | 0.268x | 16.20 | 0.648 | 1.00 | 1 / 1 | 1 |
| Genetic | 0.668 | 0.042 | 1.261x | 0.012 | 0.393x | 10.33 | 0.413 | 1.23 | 2 / 1 | 4 |

## Scenario: Uniform Random, Random-Length Ranges

Uniform random ranges over the whole array.

Setup queries: `16,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.015 | 0.038 | 1.000x | 0.011 | 1.000x | 7.69 | 0.307 | 2.99 | 1 / 1 | 0 |
| Dynamic DP | 0.015 | 0.041 | 0.924x | 0.015 | 0.750x | 8.26 | 0.330 | 2.99 | 1 / 1 | 0 |
| Genetic | 0.354 | 0.039 | 0.969x | 0.012 | 0.938x | 7.88 | 0.315 | 3.03 | 1 / 1 | 2 |

## Implementation Notes

The moving-window case exposed a real runtime issue: both adaptive lock
implementations used a linear scan through partition cuts to map a block to a
mutex. That was cheap for left-side hotspots, but expensive when the hot window
moved across the whole array. The lock implementations now maintain an O(1)
`block -> mutex` lookup table and rebuild it whenever partitions change.

The hot path no longer allocates `std::vector<std::unique_lock<Mutex>>` per
request. Single-mutex requests use a direct `std::lock_guard`; multi-mutex
requests use a small range guard that locks consecutive mutexes in increasing
order and unlocks them in reverse order.

## Takeaways

| Case | Best final measured time | Comment |
|---|---|---|
| Shifted hotspot, point queries | Genetic, 1.523x over naive | Adaptive variants split the hot region |
| Clustered 2 hot windows | Genetic, 1.655x over naive | Compact cold/hot/cold/hot/cold layout |
| Clustered 4 hot windows | Genetic, 1.402x over naive | Compact cold/hot repeated layout; Dynamic also benefits |
| Moving small window | Genetic, 1.188x over naive | Slow per-stop adaptation lets the window repartition before measurement |
| Uniform random, point queries | Naive | No stable locality to exploit |
| Shifted hotspot, random ranges | Genetic, 1.277x over naive | Dynamic keeps ranges coarse; genetic wins on body time but loses on empty-body time |
| Uniform random, random ranges | Naive | Multi-mutex requests exist, but no stable locality to exploit |
