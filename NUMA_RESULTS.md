# NUMA Benchmark Results

Host: 4 NUMA nodes, 128 logical CPUs, 256 GB RAM. Each NUMA node has 32 logical
CPUs. Node distances:

| From/to | 0 | 1 | 2 | 3 |
|---|---:|---:|---:|---:|
| 0 | 10 | 21 | 31 | 21 |
| 1 | 21 | 10 | 21 | 31 |
| 2 | 31 | 21 | 10 | 21 |
| 3 | 21 | 31 | 21 | 10 |

Raw copied outputs are stored in `BENCHMARK_RESULT/numa_reports/`.

Common settings:

| Parameter | Value |
|---|---:|
| Array size | 1,048,576 |
| Mutexes | 64 |
| Fine blocks | 1,024 |
| Fine block size | 1,024 |
| Rebuild interval | 10,000 ms |
| Stats sample rate | 64 |
| Genetic training batch | 100,000 |
| Genetic sample rate | 64 |
| Genetic probe gap | 1,000,000 |

Placements:

| Placement | Command shape |
|---|---|
| `node0` | `numactl --cpunodebind=0 --membind=0` |
| `nodes01` | `numactl --cpunodebind=0,1 --interleave=0,1` |
| `nodes0123` | `numactl --cpunodebind=0,1,2,3 --interleave=0,1,2,3` |

## Main Results

These are the results to use as the main NUMA presentation table.

| Placement | Threads | Scenario | Divisor | Naive total_s | Dynamic total_x | Genetic total_x |
|---|---:|---|---:|---:|---:|---:|
| node0 | 32 | `shift_hotspot_point` | 1 | 60.373 | 1.863x | 1.859x |
| node0 | 32 | `moving_small_window` | 20 | 33.060 | 1.158x | 1.291x |
| nodes01 | 32 | `shift_hotspot_point` | 4 | 18.845 | 1.191x | 1.124x |
| nodes01 | 32 | `moving_small_window` | 20 | 37.616 | 1.261x | 1.304x |
| nodes01 | 64 | `shift_hotspot_point` | 4 | 23.257 | 0.913x | 1.043x |
| nodes01 | 64 | `moving_small_window` | 40 | 26.962 | 0.993x | 1.018x |
| nodes0123 | 128 | `shift_hotspot_point` | 8 | 12.070 | 0.920x | 0.959x |
| nodes0123 | 128 | `moving_small_window` | 80 | 16.677 | 1.221x | 0.879x |
| nodes0123 | 128 | `moving_small_window` | 40 | 32.961 | 1.075x | 0.922x |

## Contention Details

`lock_sum_s` is the sum of per-thread lock wait time. When it is much larger
than wall time, the run is dominated by waiting on shared lock state.

| Placement | Threads | Scenario | Divisor | Naive lock_sum_s | Dynamic lock_sum_s | Genetic lock_sum_s |
|---|---:|---|---:|---:|---:|---:|
| nodes01 | 64 | `shift_hotspot_point` | 4 | 753.481 | 1119.740 | 1067.445 |
| nodes01 | 64 | `moving_small_window` | 40 | 902.870 | 913.163 | 1027.588 |
| nodes0123 | 128 | `shift_hotspot_point` | 8 | 745.181 | 964.524 | 1103.993 |
| nodes0123 | 128 | `moving_small_window` | 80 | 1040.054 | 879.680 | 1265.210 |
| nodes0123 | 128 | `moving_small_window` | 40 | 2060.328 | 1978.666 | 2579.237 |
