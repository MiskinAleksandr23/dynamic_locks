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

Current benchmark output names: `total_s` is "Total time", `total_x` is "Total
speedup", `measured_s` is "Measured phase time", `lock_only_s` is "Empty
critical section time", and `lock_x` is "Empty-body speedup".

## ARM/macOS Results

No ARM/macOS snapshot is recorded here yet. Re-run the current benchmark on the
ARM machine and add the fresh results in the same online `total_s` / `total_x`
format as the x86/Linux section.

## x86/Linux Server Configuration

Date: 2026-06-23 UTC.

Host: `x86_64`, Linux `6.8.0-117-generic`, 8 vCPUs, `AMD EPYC Processor`
under KVM/QEMU.

Build: clang++ 18.1.3, `CMAKE_BUILD_TYPE=Release`,
`BUILD_BENCHMARK_V700=ON`.

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

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 36.940 | 1.000x | 6.010 | 30.925 | 34.483 | 1.000x | 6.32 | 459.777 | 1.00 | 1 / 1 |
| Dynamic DP | 13.073 | 2.826x | 2.723 | 10.316 | 14.534 | 2.373x | 1.44 | 104.707 | 1.00 | 16 / 16 |
| Genetic | 19.458 | 1.898x | 5.070 | 14.381 | 17.467 | 1.974x | 2.18 | 158.490 | 1.00 | 16 / 16 |

### Scenario: Clustered 2 Hot Windows

Setup queries: `3,200,000 + 9,600,000`. Measured queries: `60,000,000`.
Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 32.111 | 1.000x | 5.578 | 26.518 | 25.999 | 1.000x | 5.54 | 402.991 | 1.00 | 1 / 1 |
| Dynamic DP | 22.147 | 1.450x | 3.969 | 18.130 | 20.775 | 1.251x | 2.77 | 201.753 | 1.00 | 16 / 1 |
| Genetic | 17.608 | 1.824x | 4.366 | 13.229 | 16.455 | 1.580x | 2.32 | 168.939 | 1.00 | 16 / 1 |

### Scenario: Clustered 4 Hot Windows

Setup queries: `6,400,000 + 19,200,000`. Measured queries: `120,000,000`.
Query divisor: `1`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 44.562 | 1.000x | 8.066 | 36.483 | 43.330 | 1.000x | 3.84 | 559.058 | 1.00 | 1 / 1 |
| Dynamic DP | 28.191 | 1.581x | 5.595 | 22.547 | 28.926 | 1.498x | 1.44 | 209.506 | 1.00 | 1 / 1 |
| Genetic | 33.879 | 1.315x | 7.206 | 26.659 | 31.540 | 1.374x | 1.73 | 252.612 | 1.00 | 1 / 1 |

The `Hot locks L/R` diagnostic is not meaningful for compact clustered
scenarios because it reports fixed left/right windows, not the internal cluster
windows.

### Scenario: Clustered Churn, 2 Hot Windows

Setup queries: `6,400,000 + 48,000,000 timed adapt`. Measured queries:
`100,000,000`. Query divisor: `1`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 33.326 | 1.000x | 11.499 | 21.779 | 31.784 | 1.000x | 5.36 | 413.714 | 1.00 | 1 / 1 |
| Dynamic DP | 22.962 | 1.451x | 8.709 | 14.182 | 20.550 | 1.547x | 2.83 | 218.762 | 1.00 | 16 / 1 |
| Genetic | 20.562 | 1.621x | 9.427 | 11.084 | 20.586 | 1.544x | 2.76 | 213.259 | 1.00 | 16 / 1 |

### Scenario: Clustered Churn, 4 Hot Windows

Setup queries: `3,200,000 + 24,000,000 timed adapt`. Measured queries:
`50,000,000`. Query divisor: `2`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 49.059 | 1.000x | 17.535 | 31.477 | 42.528 | 1.000x | 4.03 | 621.881 | 1.00 | 1 / 1 |
| Dynamic DP | 34.369 | 1.427x | 13.899 | 20.395 | 29.829 | 1.426x | 1.93 | 297.405 | 1.00 | 1 / 1 |
| Genetic | 37.792 | 1.298x | 17.170 | 20.574 | 34.050 | 1.249x | 2.30 | 354.764 | 1.00 | 1 / 1 |

### Scenario: Moving Small Window

Setup queries: `640,000 + 13,440,000 timed adapt`. Measured queries:
`49,000,000`. Query divisor: `20`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 64.196 | 1.000x | 14.406 | 49.608 | 33.247 | 1.000x | 12.19 | 768.755 | 1.00 | 1 / 1 |
| Dynamic DP | 36.868 | 1.741x | 9.253 | 27.404 | 25.547 | 1.301x | 7.23 | 456.100 | 1.00 | 16 / 1 |
| Genetic | 48.709 | 1.318x | 13.336 | 35.186 | 28.253 | 1.177x | 9.35 | 589.865 | 1.00 | 16 / 1 |

### Scenario: Uniform Random, Point Queries

Setup queries: `3,200,000`. Measured queries: `60,000,000`.
Query divisor: `2`. Conservative uniform settings:
`DYNAMIC_LOCK_STATS_SAMPLE_RATE=1024`,
`DYNAMIC_LOCK_REBUILD_MIN_SKEW=1000`,
`DYNAMIC_LOCK_GENETIC_TRAINING_SAMPLE_RATE=1024`,
`DYNAMIC_LOCK_GENETIC_PROBE_GAP=1000000000`,
`DYNAMIC_LOCK_GENETIC_MIN_SKEW=1000`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 12.521 | 1.000x | 0.634 | 11.884 | 10.774 | 1.000x | 0.57 | 36.072 | 1.00 | 1 / 1 |
| Dynamic DP | 13.564 | 0.923x | 0.727 | 12.825 | 13.106 | 0.822x | 1.73 | 109.597 | 1.00 | 1 / 1 |
| Genetic | 14.205 | 0.881x | 0.638 | 13.562 | 13.286 | 0.811x | 1.31 | 82.837 | 1.00 | 1 / 1 |

### Scenario: Shifted Hotspot, Random-Length Ranges

Setup queries: `1,600,000 + 4,800,000`. Measured queries: `4,000,000`.
Query divisor: `1`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 27.214 | 1.000x | 16.866 | 10.333 | 5.854 | 1.000x | 37.16 | 386.443 | 1.00 | 1 / 1 |
| Dynamic DP | 26.203 | 1.039x | 16.288 | 9.895 | 6.239 | 0.938x | 35.69 | 371.169 | 1.00 | 1 / 1 |
| Genetic | 24.926 | 1.092x | 15.980 | 8.929 | 5.602 | 1.045x | 34.14 | 355.095 | 4.05 | 16 / 16 |

### Scenario: Uniform Random, Random-Length Ranges

Setup queries: `1,600,000`. Measured queries: `4,000,000`.
Query divisor: `1`.

| Implementation | Total time, s | Total speedup | Setup time, s | Measured phase time, s | Empty critical section time, s | Empty-body speedup | Avg lock wait, us | Total lock wait, s | Avg mutexes/query | Hot locks L/R |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Naive fixed | 21.881 | 1.000x | 6.283 | 15.587 | 4.938 | 1.000x | 52.50 | 293.994 | 3.00 | 1 / 1 |
| Dynamic DP | 21.648 | 1.011x | 6.057 | 15.571 | 5.362 | 0.921x | 51.95 | 290.917 | 3.00 | 1 / 1 |
| Genetic | 21.575 | 1.014x | 6.240 | 15.324 | 4.981 | 0.991x | 51.69 | 289.472 | 3.00 | 1 / 1 |

## x86/Linux Server Takeaways

| Case | Total speedup | Comment |
|---|---:|---|
| Shifted hotspot, point queries | Dynamic 2.826x, Genetic 1.898x | Strongest locality case; full rebuild/training cost is included |
| Clustered 2 hot windows | Dynamic 1.450x, Genetic 1.824x | Both variants remain positive, genetic is faster in this run |
| Clustered 4 hot windows | Dynamic 1.581x, Genetic 1.315x | Larger honest run amortizes one training/repartitioning pass |
| Clustered churn, 2 hot windows | Dynamic 1.451x, Genetic 1.621x | Online repartitioning/training remains positive across changing windows |
| Clustered churn, 4 hot windows | Dynamic 1.427x, Genetic 1.298x | Larger honest run amortizes churn-phase rebuild/training cost |
| Moving small window | Dynamic 1.741x, Genetic 1.318x | Moving locality remains positive with all online adaptation included |
| Uniform random, point queries | Dynamic 0.923x, Genetic 0.881x | Conservative uniform settings avoid useless rebuild/training; no stable locality remains a weak case |
| Shifted hotspot, random ranges | Dynamic 1.039x, Genetic 1.092x | Body work dominates; gains are small |
| Uniform random, random ranges | Dynamic 1.011x, Genetic 1.014x | No stable locality; result is effectively neutral |
