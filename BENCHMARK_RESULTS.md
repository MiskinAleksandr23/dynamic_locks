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
| Main tables lock primitive | `std::mutex` |

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
| Naive fixed | 0.057 | 0.253 | 1.000x | 0.181 | 1.000x | 1.48 | 1.773 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.379 | 0.184 | 1.378x | 0.161 | 1.123x | 1.13 | 1.360 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.702 | 0.147 | 1.716x | 0.145 | 1.244x | 0.93 | 1.119 | 1.00 | 16 / 1 | 7 |

Result: both adaptive implementations split the left hot window across 16 mutexes. This is beneficial for point requests because each measured query still acquires one mutex, but contention is spread across the hot window. Genetic has higher setup cost, but the fastest final phase after convergence.

### Dynamic DP: `std::mutex` vs `spinlock`

Same shifted-hotspot point scenario, only for `Dynamic DP`. Values above `1.0x` in the last two columns mean `spinlock` is faster than `std::mutex`.

| Lock primitive | Measured request time, s | Empty critical section time, s | Measured speed vs `std::mutex` | Empty speed vs `std::mutex` |
|---|---:|---:|---:|---:|
| `std::mutex` | 0.184 | 0.161 | 1.000x | 1.000x |
| `alignas(64) spinlock` | 0.258 | 0.249 | 0.713x | 0.646x |

Result: in this point-query case the simple yielding spinlock is slower than `std::mutex` for `Dynamic DP`, even with cache-line alignment.

## Scenario: Uniform Random, Point Queries

Warmup and final measured requests are uniformly random point updates over the whole array.

Setup queries: `64,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.011 | 0.182 | 1.000x | 0.173 | 1.000x | 0.76 | 0.916 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.009 | 0.178 | 1.028x | 0.183 | 0.947x | 0.78 | 0.932 | 1.00 | 1 / 1 | 0 |
| Genetic | 0.400 | 0.152 | 1.201x | 0.148 | 1.170x | 0.78 | 0.941 | 1.00 | 1 / 2 | 3 |

Result: this run has Genetic ahead on final measured time, but there is still no stable hotspot to exploit. Treat this scenario as mostly runtime overhead and scheduling noise rather than evidence of useful adaptation.

## Scenario: Shifted Hotspot, Random-Length Ranges

Right-side warmup, then workload shifts to the left hot window. Final measured requests are ranges inside the left hot window. Range length is random; because the hot window is `16,384` elements, lengths are effectively capped by that window even though the global max is `65,536`.

Setup queries: `16,000 + 48,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.093 | 0.056 | 1.000x | 0.006 | 1.000x | 11.06 | 0.442 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.415 | 0.085 | 0.660x | 0.020 | 0.296x | 17.11 | 0.684 | 1.00 | 1 / 1 | 1 |
| Genetic | 0.632 | 0.046 | 1.232x | 0.013 | 0.448x | 11.05 | 0.442 | 1.23 | 2 / 1 | 4 |

Result: this case shows the range-query tradeoff. Naive and Dynamic DP keep the left hot window coarse, so measured range requests acquire one mutex on average. Genetic uses a slightly finer split (`2 / 1` hot locks), which raises average mutexes per query to `1.23`, but it still has the fastest measured request time in this run. For empty critical sections, all adaptive variants remain slower than naive because setup/adaptation and multi-lock overhead are not offset by useful work.

## Scenario: Uniform Random, Random-Length Ranges

Warmup and final measured requests are uniformly random ranges over the whole array with random length from `1` to `65,536`.

Setup queries: `16,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.016 | 0.038 | 1.000x | 0.011 | 1.000x | 7.56 | 0.302 | 2.99 | 1 / 1 | 0 |
| Dynamic DP | 0.016 | 0.043 | 0.882x | 0.018 | 0.623x | 8.51 | 0.341 | 2.99 | 1 / 1 | 0 |
| Genetic | 0.406 | 0.040 | 0.948x | 0.013 | 0.890x | 8.02 | 0.321 | 3.04 | 1 / 1 | 2 |

Result: this scenario explicitly exercises multi-mutex requests. All implementations acquire about `3` mutexes per query on average. Since the distribution is uniform, adaptive partitioning has no stable hotspot to exploit; differences are mostly runtime overhead and benchmark noise.

## Takeaways

| Case | Best final measured time | Comment |
|---|---|---|
| Shifted hotspot, point queries | Genetic, 1.716x over naive | Hot window is split into 16 locks and each point query still takes one mutex |
| Uniform random, point queries | Genetic, 1.201x over naive in this run | No concentrated hotspot; this is mostly runtime and scheduling noise |
| Shifted hotspot, random-length ranges | Genetic, 1.232x over naive | Range-aware DP keeps the hot window coarse; genetic is still fastest on body time in this run |
| Uniform random, random-length ranges | Naive | Multi-mutex requests are visible, but no adaptive strategy has structure to exploit |
| Dynamic DP point queries with spinlock | `std::mutex` is faster | Current spinlock is not a drop-in improvement for this adaptive hot path |
