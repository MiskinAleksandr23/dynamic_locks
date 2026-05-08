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
| Moving window size | 16,384 |
| Moving window stops | 14 |
| Query divisor | 1 |
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

To reduce query counts approximately by `M` at runtime, set
`DYNAMIC_LOCK_QUERY_DIVISOR=M`. The default is `1`. For example:

```bash
DYNAMIC_LOCK_QUERY_DIVISOR=10 ./cmake-build-release/adaptive_lock_benchmark
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

Setup queries: `6,400,000 + 19,200,000`. Measured queries: `120,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 4.753 | 21.431 | 1.000x | 13.475 | 1.000x | 1.26 | 151.006 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 4.317 | 13.416 | 1.597x | 12.503 | 1.078x | 0.89 | 106.271 | 1.00 | 16 / 1 | 3 |
| Genetic | 7.950 | 13.137 | 1.631x | 12.301 | 1.095x | 0.87 | 104.439 | 1.00 | 16 / 1 | 97 |

## Scenario: Clustered 2 Hot Windows

Two compact 16,384-element hot windows. Requests are point updates. The hot
windows start at fine blocks `16` and `48`, so there is a cold gap before,
between, and after the hot intervals.

Setup queries: `6,400,000 + 19,200,000`. Measured queries: `120,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 3.617 | 16.924 | 1.000x | 14.795 | 1.000x | 1.08 | 129.007 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 5.320 | 11.850 | 1.428x | 12.174 | 1.215x | 0.75 | 90.536 | 1.00 | 16 / 1 | 1 |
| Genetic | 9.653 | 11.869 | 1.426x | 11.470 | 1.290x | 0.75 | 90.458 | 1.00 | 2 / 1 | 79 |

## Scenario: Clustered 4 Hot Windows

Four compact 16,384-element hot windows. Requests are point updates. The hot
windows start at fine blocks `16`, `48`, `80`, and `112`, giving the intended
cold/hot/cold/hot/cold/hot/cold/hot/cold structure.

Setup queries: `6,400,000 + 19,200,000`. Measured queries: `120,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 3.141 | 15.203 | 1.000x | 14.061 | 1.000x | 0.97 | 116.713 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 5.019 | 12.574 | 1.209x | 12.153 | 1.157x | 0.67 | 80.698 | 1.00 | 1 / 1 | 1 |
| Genetic | 12.146 | 11.998 | 1.267x | 11.936 | 1.178x | 0.66 | 79.172 | 1.00 | 1 / 1 | 76 |

Result: the old 1,024-element clustered windows were exactly one fine block, so
adaptive implementations could not split inside a window. With 16,384-element
windows, each hot interval contains 16 fine blocks. The `Hot locks L/R` column
is not meaningful for compact clustered scenarios because it still reports the
fixed left/right 16,384-element diagnostic windows, not the cluster windows.

## Scenario: Clustered Churn, 2 Hot Windows

Two compact 16,384-element hot windows. There are 5 timed phases; in every
phase, half of the active windows is replaced by new compact windows before
measuring point requests.

Setup queries: `6,400,000 + 48,000,000 timed adapt`. Measured queries: `100,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 7.918 | 14.897 | 1.000x | 12.386 | 1.000x | 1.13 | 112.651 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 7.999 | 9.923 | 1.501x | 9.613 | 1.288x | 0.75 | 74.992 | 1.00 | 16 / 1 | 20 |
| Genetic | 19.242 | 9.850 | 1.512x | 9.603 | 1.290x | 0.75 | 75.138 | 1.00 | 1 / 1 | 156 |

## Scenario: Clustered Churn, 4 Hot Windows

Four compact 16,384-element hot windows. There are 5 timed phases; in every
phase, half of the active windows is replaced by new compact windows before
measuring point requests.

Setup queries: `6,400,000 + 48,000,000 timed adapt`. Measured queries: `100,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 6.890 | 12.785 | 1.000x | 12.007 | 1.000x | 0.99 | 98.631 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 7.711 | 9.901 | 1.291x | 9.750 | 1.231x | 0.66 | 65.536 | 1.00 | 1 / 1 | 20 |
| Genetic | 22.648 | 10.368 | 1.233x | 9.828 | 1.222x | 0.67 | 67.421 | 1.00 | 1 / 1 | 139 |

## Scenario: Moving Small Window

A 16,384-element window moves left-to-right and back. For each stop, the
benchmark adapts/rebuilds first, then measures with online adaptation disabled.
This checks whether the learned layout is useful after the window has moved
slowly enough to converge.

Setup queries: `12,800,000 + 268,800,000 timed adapt`. Measured queries:
`980,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 51.986 | 180.355 | 1.000x | 113.022 | 1.000x | 1.27 | 1249.434 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 43.041 | 107.076 | 1.684x | 97.033 | 1.165x | 0.81 | 791.501 | 1.00 | 16 / 1 | 53 |
| Genetic | 76.518 | 109.463 | 1.648x | 98.245 | 1.150x | 0.83 | 813.699 | 1.00 | 1 / 1 | 1021 |

Result: after fixing runtime lookup to O(1), the moving-window case shows a
measured win for both adaptive implementations. The setup cost is intentionally
higher because every window stop gets its own adaptation phase.

## Scenario: Uniform Random, Point Queries

Uniform random point updates over the whole array.

Setup queries: `6,400,000`. Measured queries: `120,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.636 | 11.473 | 1.000x | 12.326 | 1.000x | 0.60 | 71.533 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.817 | 12.484 | 0.919x | 12.158 | 1.014x | 0.67 | 80.406 | 1.00 | 1 / 1 | 0 |
| Genetic | 4.652 | 12.036 | 0.953x | 11.900 | 1.036x | 0.67 | 79.946 | 1.00 | 1 / 2 | 29 |

## Scenario: Shifted Hotspot, Random-Length Ranges

Right-side warmup, then left hot-window random-length ranges.

Setup queries: `1,600,000 + 4,800,000`. Measured queries: `4,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 8.640 | 5.453 | 1.000x | 0.446 | 1.000x | 10.81 | 43.245 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 11.483 | 6.411 | 0.851x | 1.829 | 0.244x | 15.86 | 63.422 | 3.40 | 5 / 1 | 4 |
| Genetic | 11.604 | 5.321 | 1.025x | 2.806 | 0.159x | 13.44 | 53.742 | 8.96 | 16 / 1 | 87 |

## Scenario: Uniform Random, Random-Length Ranges

Uniform random ranges over the whole array.

Setup queries: `1,600,000`. Measured queries: `4,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 1.480 | 3.669 | 1.000x | 1.120 | 1.000x | 7.53 | 30.121 | 3.00 | 1 / 1 | 0 |
| Dynamic DP | 1.493 | 3.673 | 0.999x | 1.131 | 0.990x | 7.50 | 30.017 | 3.00 | 1 / 1 | 0 |
| Genetic | 2.180 | 3.654 | 1.004x | 1.100 | 1.018x | 7.46 | 29.859 | 3.04 | 1 / 1 | 9 |

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
| Shifted hotspot, point queries | Genetic, 1.631x over naive | Adaptive variants split the hot region |
| Clustered 2 hot windows | Dynamic DP, 1.428x over naive | Compact cold/hot/cold/hot/cold layout; Dynamic and Genetic are effectively tied |
| Clustered 4 hot windows | Genetic, 1.267x over naive | Compact cold/hot repeated layout; both adaptive implementations benefit |
| Clustered churn, 2 hot windows | Genetic, 1.512x over naive | Half of the active windows changes each phase |
| Clustered churn, 4 hot windows | Dynamic DP, 1.291x over naive | More windows dilute per-window contention, but the effect remains stable |
| Moving small window | Dynamic DP, 1.684x over naive | 16-page moving window gives the strongest stable win |
| Uniform random, point queries | Naive | No stable locality to exploit |
| Shifted hotspot, random ranges | Genetic, 1.025x over naive | Adaptive splitting increases mutexes per range and hurts empty-body time |
| Uniform random, random ranges | Genetic, 1.004x over naive | Effectively parity; no stable locality to exploit |
