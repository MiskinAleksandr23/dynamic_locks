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
| Naive fixed | 0.042 | 0.201 | 1.000x | 0.133 | 1.000x | 1.15 | 1.377 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.356 | 0.156 | 1.286x | 0.146 | 0.912x | 1.01 | 1.210 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.710 | 0.150 | 1.334x | 0.139 | 0.957x | 0.97 | 1.166 | 1.00 | 16 / 1 | 7 |

## Scenario: Clustered 2 Hot Windows

Two compact 16,384-element hot windows. Requests are point updates. The hot
windows start at fine blocks `16` and `48`, so there is a cold gap before,
between, and after the hot intervals.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.038 | 0.180 | 1.000x | 0.148 | 1.000x | 1.15 | 1.376 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.353 | 0.146 | 1.233x | 0.138 | 1.074x | 0.90 | 1.075 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.756 | 0.137 | 1.313x | 0.131 | 1.130x | 0.84 | 1.014 | 1.00 | 1 / 1 | 7 |

## Scenario: Clustered 4 Hot Windows

Four compact 16,384-element hot windows. Requests are point updates. The hot
windows start at fine blocks `16`, `48`, `80`, and `112`, giving the intended
cold/hot/cold/hot/cold/hot/cold/hot/cold structure.

Setup queries: `64,000 + 192,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.033 | 0.154 | 1.000x | 0.138 | 1.000x | 1.01 | 1.208 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.346 | 0.152 | 1.015x | 0.145 | 0.952x | 0.82 | 0.981 | 1.00 | 1 / 1 | 1 |
| Genetic | 0.960 | 0.139 | 1.112x | 0.133 | 1.038x | 0.78 | 0.936 | 1.00 | 1 / 1 | 7 |

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
| Naive fixed | 0.080 | 0.150 | 1.000x | 0.122 | 1.000x | 1.15 | 1.145 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.577 | 0.118 | 1.278x | 0.112 | 1.091x | 0.86 | 0.855 | 1.00 | 16 / 1 | 5 |
| Genetic | 1.988 | 0.124 | 1.214x | 0.120 | 1.017x | 0.89 | 0.889 | 1.00 | 1 / 1 | 19 |

## Scenario: Clustered Churn, 4 Hot Windows

Four compact 16,384-element hot windows. There are 5 timed phases; in every
phase, half of the active windows is replaced by new compact windows before
measuring point requests.

Setup queries: `64,000 + 480,000 timed adapt`. Measured queries: `1,000,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.069 | 0.129 | 1.000x | 0.113 | 1.000x | 1.01 | 1.014 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.585 | 0.122 | 1.055x | 0.118 | 0.964x | 0.76 | 0.762 | 1.00 | 1 / 1 | 5 |
| Genetic | 2.345 | 0.125 | 1.034x | 0.119 | 0.950x | 0.81 | 0.805 | 1.00 | 1 / 1 | 18 |

## Scenario: Moving Small Window

A 16,384-element window moves left-to-right and back. For each stop, the
benchmark adapts/rebuilds first, then measures with online adaptation disabled.
This checks whether the learned layout is useful after the window has moved
slowly enough to converge.

Setup queries: `128,000 + 2,688,000 timed adapt`. Measured queries:
`9,800,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.490 | 1.732 | 1.000x | 1.106 | 1.000x | 1.21 | 11.857 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 1.271 | 1.270 | 1.364x | 1.112 | 0.994x | 1.04 | 10.143 | 1.00 | 16 / 1 | 14 |
| Genetic | 5.380 | 1.279 | 1.354x | 1.152 | 0.960x | 1.02 | 9.988 | 1.00 | 1 / 1 | 70 |

Result: after fixing runtime lookup to O(1), the moving-window case shows a
measured win for both adaptive implementations. The setup cost is intentionally
higher because every window stop gets its own adaptation phase.

## Scenario: Uniform Random, Point Queries

Uniform random point updates over the whole array.

Setup queries: `64,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.007 | 0.120 | 1.000x | 0.120 | 1.000x | 0.65 | 0.778 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.011 | 0.138 | 0.867x | 0.121 | 0.994x | 0.73 | 0.874 | 1.00 | 1 / 1 | 0 |
| Genetic | 0.414 | 0.141 | 0.851x | 0.134 | 0.898x | 0.74 | 0.892 | 1.00 | 1 / 1 | 3 |

## Scenario: Shifted Hotspot, Random-Length Ranges

Right-side warmup, then left hot-window random-length ranges.

Setup queries: `16,000 + 48,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.088 | 0.054 | 1.000x | 0.004 | 1.000x | 10.49 | 0.420 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.410 | 0.079 | 0.682x | 0.011 | 0.411x | 16.12 | 0.645 | 1.00 | 1 / 1 | 1 |
| Genetic | 0.657 | 0.043 | 1.240x | 0.012 | 0.367x | 10.57 | 0.423 | 1.23 | 2 / 1 | 4 |

## Scenario: Uniform Random, Random-Length Ranges

Uniform random ranges over the whole array.

Setup queries: `16,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.016 | 0.038 | 1.000x | 0.011 | 1.000x | 7.60 | 0.304 | 2.99 | 1 / 1 | 0 |
| Dynamic DP | 0.015 | 0.037 | 1.013x | 0.017 | 0.610x | 7.50 | 0.300 | 2.99 | 1 / 1 | 0 |
| Genetic | 0.409 | 0.040 | 0.953x | 0.012 | 0.853x | 7.89 | 0.316 | 3.03 | 1 / 1 | 2 |

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
| Shifted hotspot, point queries | Genetic, 1.334x over naive | Adaptive variants split the hot region |
| Clustered 2 hot windows | Genetic, 1.313x over naive | Compact cold/hot/cold/hot/cold layout |
| Clustered 4 hot windows | Genetic, 1.112x over naive | Compact cold/hot repeated layout; Dynamic is close to parity here |
| Clustered churn, 2 hot windows | Dynamic DP, 1.278x over naive | Half of the active windows changes each phase |
| Clustered churn, 4 hot windows | Dynamic DP, 1.055x over naive | More windows dilute per-window contention |
| Moving small window | Dynamic DP, 1.364x over naive | 16-page moving window gives adaptive implementations enough internal structure |
| Uniform random, point queries | Naive | No stable locality to exploit |
| Shifted hotspot, random ranges | Genetic, 1.277x over naive | Dynamic keeps ranges coarse; genetic wins on body time but loses on empty-body time |
| Uniform random, random ranges | Naive | Multi-mutex requests exist, but no stable locality to exploit |
