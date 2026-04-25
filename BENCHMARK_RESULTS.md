# Dynamic Lock Benchmark Results

Snapshot from `./cmake-build-release/adaptive_lock_benchmark`.

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
| Naive fixed | 0.062 | 0.280 | 1.000x | 0.187 | 1.000x | 1.69 | 2.025 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.376 | 0.177 | 1.581x | 0.159 | 1.174x | 1.11 | 1.334 | 1.00 | 16 / 1 | 1 |
| Genetic | 0.734 | 0.149 | 1.888x | 0.138 | 1.355x | 0.94 | 1.131 | 1.00 | 16 / 1 | 7 |

Result: both adaptive implementations split the left hot window across 16 mutexes. This is beneficial for point requests because each measured query still acquires one mutex, but contention is spread across the hot window. Genetic has higher setup cost, but the fastest final phase after convergence.

### Dynamic DP: `std::mutex` vs `spinlock`

Same shifted-hotspot point scenario, only for `Dynamic DP`. Values above `1.0x` in the last two columns mean `spinlock` is faster than `std::mutex`.

| Lock primitive | Measured request time, s | Empty critical section time, s | Measured speed vs `std::mutex` | Empty speed vs `std::mutex` |
|---|---:|---:|---:|---:|
| `std::mutex` | 0.177 | 0.159 | 1.000x | 1.000x |
| `alignas(64) spinlock` | 0.254 | 0.248 | 0.699x | 0.642x |

Result: in this point-query case the simple yielding spinlock is slower than `std::mutex` for `Dynamic DP`, even with cache-line alignment.

## Scenario: Uniform Random, Point Queries

Warmup and final measured requests are uniformly random point updates over the whole array.

Setup queries: `64,000`. Measured queries: `1,200,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.007 | 0.132 | 1.000x | 0.129 | 1.000x | 0.70 | 0.836 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.013 | 0.149 | 0.888x | 0.147 | 0.881x | 0.78 | 0.938 | 1.00 | 1 / 1 | 0 |
| Genetic | 0.410 | 0.154 | 0.862x | 0.155 | 0.833x | 0.79 | 0.946 | 1.00 | 1 / 2 | 3 |

Result: adaptive partitioning does not help on uniform random point requests. There is no stable hotspot to exploit, so the extra adaptation/runtime overhead dominates.

## Scenario: Shifted Hotspot, Random-Length Ranges

Right-side warmup, then workload shifts to the left hot window. Final measured requests are ranges inside the left hot window. Range length is random; because the hot window is `16,384` elements, lengths are effectively capped by that window even though the global max is `65,536`.

Setup queries: `16,000 + 48,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.088 | 0.054 | 1.000x | 0.006 | 1.000x | 10.55 | 0.422 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 0.414 | 0.087 | 0.625x | 0.011 | 0.579x | 18.15 | 0.726 | 1.00 | 1 / 1 | 1 |
| Genetic | 0.652 | 0.045 | 1.211x | 0.015 | 0.421x | 10.85 | 0.434 | 1.23 | 2 / 1 | 4 |

Result: this case shows the range-query tradeoff. Naive and Dynamic DP keep the left hot window coarse, so measured range requests acquire one mutex on average. Genetic uses a slightly finer split (`2 / 1` hot locks), which raises average mutexes per query to `1.23`, but it still has the fastest measured request time in this run. For empty critical sections, all adaptive variants remain slower than naive because setup/adaptation and multi-lock overhead are not offset by useful work.

## Scenario: Uniform Random, Random-Length Ranges

Warmup and final measured requests are uniformly random ranges over the whole array with random length from `1` to `65,536`.

Setup queries: `16,000`. Measured queries: `40,000`.

| Implementation | Setup time, s | Measured request time, s | Request speedup | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 0.015 | 0.037 | 1.000x | 0.012 | 1.000x | 7.35 | 0.294 | 2.99 | 1 / 1 | 0 |
| Dynamic DP | 0.016 | 0.043 | 0.851x | 0.016 | 0.700x | 8.75 | 0.350 | 2.99 | 1 / 1 | 0 |
| Genetic | 0.382 | 0.039 | 0.941x | 0.013 | 0.871x | 7.80 | 0.312 | 3.03 | 1 / 1 | 2 |

Result: this scenario explicitly exercises multi-mutex requests. All implementations acquire about `3` mutexes per query on average. Since the distribution is uniform, adaptive partitioning has no stable hotspot to exploit; differences are mostly runtime overhead and benchmark noise.

## Takeaways

| Case | Best final measured time | Comment |
|---|---|---|
| Shifted hotspot, point queries | Genetic, 1.888x over naive | Hot window is split into 16 locks and each point query still takes one mutex |
| Uniform random, point queries | Naive | No concentrated hotspot |
| Shifted hotspot, random-length ranges | Genetic, 1.211x over naive | Range-aware DP keeps the hot window coarse; genetic is still fastest on body time in this run |
| Uniform random, random-length ranges | Naive | Multi-mutex requests are visible, but no adaptive strategy has structure to exploit |
| Dynamic DP point queries with spinlock | `std::mutex` is faster | Current spinlock is not a drop-in improvement for this adaptive hot path |
