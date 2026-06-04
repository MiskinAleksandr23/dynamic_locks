# Dynamic Lock Benchmark Results

Snapshots from `adaptive_lock_benchmark`. The current checked-in snapshot is
for x86/Linux; ARM/macOS is intentionally left empty until it is rerun on that
machine.

`Benchmark-v700` is vendored, but the top-level build does not build it unless
`-DBUILD_BENCHMARK_V700=ON` is passed.

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

Current benchmark output names: `total_s` is "Total time", `total_x` is "Total
speedup", `measured_s` is "Measured phase time", `lock_only_s` is "Empty
critical section time", and `lock_x` is "Empty-body speedup".

## ARM/macOS Results

No ARM/macOS snapshot is recorded here yet. Re-run the current benchmark on the
ARM machine and add the fresh results in the same online `total_s` / `total_x`
format as the x86/Linux section.

## x86/Linux Server Configuration

Current x86 numbers use the online benchmark path: `total_s` measures the full
scenario wall time from `StartRebuilder()` to `StopRebuilder()`. Warmup, adapt,
measured phases, online stats collection, dynamic rebuilds, genetic training,
and stop/join time are included. Queries are generated on the fly, so large runs
do not allocate a request array.

Run command pattern:

```bash
env DYNAMIC_LOCK_SCENARIO=<scenario> \
  DYNAMIC_LOCK_QUERY_DIVISOR=<divisor> \
  DYNAMIC_LOCK_THREAD_COUNT=16 \
  DYNAMIC_LOCK_REBUILD_INTERVAL_MS=500 \
  DYNAMIC_LOCK_STATS_SAMPLE_RATE=64 \
  DYNAMIC_LOCK_REBUILD_MIN_GAIN=1.0 \
  DYNAMIC_LOCK_REBUILD_MIN_SKEW=4.0 \
  DYNAMIC_LOCK_RUN_GENETIC=1 \
  DYNAMIC_LOCK_GENETIC_TRAINING_BATCH=100000 \
  DYNAMIC_LOCK_GENETIC_TRAINING_SAMPLE_RATE=64 \
  DYNAMIC_LOCK_GENETIC_PROBE_GAP=1000000 \
  DYNAMIC_LOCK_GENETIC_MIN_SKEW=4.0 \
  ./build/adaptive_lock_benchmark
```

| Parameter | Value |
|---|---:|
| Array size | 1,048,576 |
| Mutexes | 64 |
| Fine blocks | 1,024 |
| Fine block size | 1,024 |
| Threads | 16 |
| Point query length | 1 |
| Random range max length | 65,536 |
| Hot window size | 16,384 |
| Clustered hot window size | 16,384 |
| Clustered churn changes | 5 |
| Small range max length | 8 |
| Moving window size | 16,384 |
| Moving window stops | 14 |
| Dynamic rebuild interval | 500 ms |
| Dynamic rebuild threshold | 2.0 |
| Dynamic rebuild min gain | 1.0 |
| Dynamic rebuild min skew | 4.0 |
| Dynamic stats sample rate | 64 |
| Genetic | Enabled |
| Genetic training batch | 100,000 |
| Genetic training sample rate | 64 |
| Genetic training probe gap | 1,000,000 operations |
| Genetic min training skew | 4.0 |
| Main lock primitive | `std::mutex` |

## x86/Linux Server Results

### Scenario: Shifted Hotspot, Point Queries

Setup queries: `3,200,000 + 9,600,000`. Measured queries: `60,000,000`.
Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 33.015 | 1.000x | 5.954 | 27.053 | 31.071 | 1.000x | 5.68 | 413.290 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 13.799 | 2.393x | 2.500 | 11.270 | 13.224 | 2.350x | 1.91 | 138.972 | 1.00 | 16 / 16 | 1 |
| Genetic | 15.622 | 2.113x | 4.135 | 11.479 | 14.396 | 2.158x | 2.14 | 155.807 | 1.00 | 16 / 16 | 1 |

### Scenario: Clustered 2 Hot Windows

Setup queries: `3,200,000 + 9,600,000`. Measured queries: `60,000,000`.
Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 27.015 | 1.000x | 4.892 | 22.105 | 25.186 | 1.000x | 4.74 | 345.070 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 9.967 | 2.711x | 2.031 | 7.867 | 9.700 | 2.596x | 1.21 | 88.155 | 1.00 | 16 / 1 | 1 |
| Genetic | 15.539 | 1.739x | 4.061 | 11.463 | 13.454 | 1.872x | 1.82 | 132.799 | 1.00 | 16 / 1 | 1 |

### Scenario: Clustered 4 Hot Windows

Setup queries: `3,200,000 + 9,600,000`. Measured queries: `60,000,000`.
Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 17.528 | 1.000x | 3.386 | 14.129 | 16.425 | 1.000x | 3.00 | 218.660 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 11.711 | 1.497x | 2.218 | 9.450 | 11.433 | 1.437x | 1.64 | 119.437 | 1.00 | 1 / 1 | 1 |
| Genetic | 13.169 | 1.331x | 2.841 | 10.309 | 12.179 | 1.349x | 1.74 | 126.340 | 1.00 | 1 / 1 | 1 |

The `Hot locks L/R` diagnostic is not meaningful for compact clustered
scenarios because it reports fixed left/right windows, not the internal cluster
windows. `DYNAMIC_LOCK_DEBUG_PARTITIONS=1` confirmed that dynamic assigns 16
locks to each of the four clustered hot windows.

### Scenario: Clustered Churn, 2 Hot Windows

Setup queries: `3,200,000 + 24,000,000 timed adapt`. Measured queries:
`50,000,000`. Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 23.759 | 1.000x | 8.156 | 15.548 | 21.772 | 1.000x | 3.77 | 291.168 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 11.855 | 2.004x | 5.013 | 6.757 | 11.566 | 1.882x | 1.38 | 106.224 | 1.00 | 16 / 1 | 6 |
| Genetic | 20.028 | 1.186x | 8.738 | 11.231 | 18.384 | 1.184x | 2.52 | 194.861 | 1.00 | 16 / 1 | 5 |

### Scenario: Clustered Churn, 4 Hot Windows

Setup queries: `3,200,000 + 24,000,000 timed adapt`. Measured queries:
`50,000,000`. Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 18.087 | 1.000x | 6.323 | 11.708 | 16.338 | 1.000x | 2.87 | 221.896 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 12.875 | 1.405x | 5.997 | 6.795 | 11.908 | 1.372x | 1.73 | 133.932 | 1.00 | 1 / 1 | 11 |
| Genetic | 16.057 | 1.126x | 7.859 | 8.141 | 15.090 | 1.083x | 2.20 | 169.508 | 1.00 | 1 / 1 | 9 |

### Scenario: Moving Small Window

Setup queries: `640,000 + 13,440,000 timed adapt`. Measured queries:
`49,000,000`. Query divisor: `20`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 48.490 | 1.000x | 10.792 | 37.489 | 29.505 | 1.000x | 9.46 | 596.753 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 29.018 | 1.671x | 8.700 | 20.065 | 21.509 | 1.372x | 5.61 | 354.135 | 1.00 | 16 / 1 | 28 |
| Genetic | 33.740 | 1.437x | 9.687 | 23.850 | 23.934 | 1.233x | 6.50 | 410.102 | 1.00 | 16 / 1 | 8 |

### Scenario: Uniform Random, Point Queries

Setup queries: `3,200,000`. Measured queries: `60,000,000`.
Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 7.665 | 1.000x | 0.421 | 7.238 | 7.654 | 1.000x | 0.34 | 21.248 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 8.630 | 0.888x | 0.403 | 8.214 | 8.240 | 0.929x | 0.42 | 26.495 | 1.00 | 1 / 1 | 0 |
| Genetic | 14.001 | 0.547x | 0.600 | 13.397 | 13.735 | 0.557x | 1.00 | 63.438 | 1.00 | 1 / 1 | 0 |

### Scenario: Shifted Hotspot, Random-Length Ranges

Setup queries: `1,600,000 + 4,800,000`. Measured queries: `4,000,000`.
Query divisor: `1`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 31.378 | 1.000x | 19.731 | 11.630 | 4.687 | 1.000x | 42.73 | 444.426 | 1.00 | 1 / 1 | 0 |
| Dynamic DP | 27.027 | 1.161x | 16.566 | 10.425 | 5.250 | 0.893x | 36.37 | 378.299 | 1.00 | 1 / 1 | 0 |
| Genetic | 26.937 | 1.165x | 17.056 | 9.864 | 5.170 | 0.907x | 37.07 | 385.579 | 4.05 | 16 / 16 | 1 |

### Scenario: Uniform Random, Random-Length Ranges

Setup queries: `1,600,000`. Measured queries: `4,000,000`.
Query divisor: `1`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R | Rebuild/train |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 32.279 | 1.000x | 9.794 | 22.473 | 5.062 | 1.000x | 79.94 | 447.674 | 3.00 | 1 / 1 | 0 |
| Dynamic DP | 29.846 | 1.082x | 8.202 | 21.619 | 5.114 | 0.990x | 73.43 | 411.212 | 3.00 | 1 / 1 | 0 |
| Genetic | 34.533 | 0.935x | 9.350 | 25.170 | 5.244 | 0.965x | 86.01 | 481.638 | 3.00 | 1 / 1 | 0 |

## x86/Linux Server Takeaways

| Case | Total speedup | Comment |
|---|---:|---|
| Shifted hotspot, point queries | Dynamic 2.393x, Genetic 2.113x | Best locality case; rebuild/training cost is fully included and amortized |
| Clustered 2 hot windows | Dynamic 2.711x, Genetic 1.739x | Both adaptive variants split the compact hot windows and reduce lock wait |
| Clustered 4 hot windows | Dynamic 1.497x, Genetic 1.331x | Four hot windows remain positive with correct per-window splitting |
| Clustered churn, 2 hot windows | Dynamic 2.004x, Genetic 1.186x | Online repartitioning remains positive across changing windows |
| Clustered churn, 4 hot windows | Dynamic 1.405x, Genetic 1.126x | Weaker contention, but both adaptive variants remain above naive |
| Moving small window | Dynamic 1.671x, Genetic 1.437x | Moving locality remains positive with all online adaptation included |
| Uniform random, point queries | Naive baseline | No stable locality; adaptive monitoring overhead is not useful |
| Shifted hotspot, random ranges | Dynamic 1.161x, Genetic 1.165x | Body work dominates; genetic splits the hot range but pays more empty-body locking |
| Uniform random, random ranges | Dynamic 1.082x, Genetic 0.935x | No stable locality; genetic monitoring overhead is not useful |
