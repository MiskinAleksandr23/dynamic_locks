# NUMA Benchmark Results

Date: 2026-06-12 UTC

Host: 4 NUMA nodes, 128 logical CPUs, 256 GB RAM. Each NUMA node has 32 logical
CPUs. Node distances:

| From/to | 0 | 1 | 2 | 3 |
|---|---:|---:|---:|---:|
| 0 | 10 | 21 | 31 | 21 |
| 1 | 21 | 10 | 21 | 31 |
| 2 | 31 | 21 | 10 | 21 |
| 3 | 21 | 31 | 21 | 10 |

Raw copied outputs are stored in `BENCHMARK_RESULT/numa_reports/`.

## Method

All results use the local `adaptive_lock_benchmark` and the honest `total_s` /
`total_x` metric. `total_s` includes setup, measured phases, online monitoring,
rebuild/training, and stop/join time. Speedups are relative to `naive` in the
same scenario and placement.

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

## Main Results

These are the results to use as the main NUMA presentation table.

| Placement | Threads | Scenario | Divisor | Naive total_s | Dynamic total_x | Genetic total_x | Dynamic rebuilds | Genetic trains |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| node0 | 32 | `shift_hotspot_point` | 1 | 60.373 | 1.863x | 1.859x | 1 | 1 |
| node0 | 32 | `moving_small_window` | 20 | 33.060 | 1.158x | 1.291x | 2 | 8 |
| nodes01 | 32 | `shift_hotspot_point` | 4 | 18.845 | 1.191x | 1.124x | 1 | 1 |
| nodes01 | 32 | `moving_small_window` | 20 | 37.616 | 1.261x | 1.304x | 2 | 8 |

## Contention Diagnostics

`lock_sum_s` is aggregate lock wait across workers. It can be much larger than
wall time when many threads wait concurrently.

| Placement | Threads | Scenario | Dynamic lock_sum_s | Genetic lock_sum_s | Note |
|---|---:|---|---:|---:|---|
| node0 | 32 | `shift_hotspot_point` | 667.979 | 690.478 | Adaptive partitioning reduces wait vs naive. |
| node0 | 32 | `moving_small_window` | 493.147 | 468.281 | Both variants reduce aggregate lock wait. |
| nodes01 | 32 | `shift_hotspot_point` | 304.352 | 375.834 | Positive speedup, but cross-node overhead is visible. |
| nodes01 | 32 | `moving_small_window` | 529.811 | 520.441 | Longer moving run remains positive for both variants. |

## Scaling Limit

Two-node 64-thread runs show the current implementation's NUMA scaling limit.

| Placement | Threads | Scenario | Divisor | Naive total_s | Dynamic total_x | Genetic total_x | Dynamic lock_sum_s | Genetic lock_sum_s |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| nodes01 | 64 | `shift_hotspot_point` | 4 | 21.023 | 0.958x | 0.939x | 966.828 | 1125.716 |
| nodes01 | 64 | `moving_small_window` | 40 | 25.160 | 1.129x | 1.021x | 789.463 | 949.366 |

The partitioning is still learned in the 64-thread shift run (`hot(L/R)` becomes
`16/16`), but the total runtime regresses. This points to cross-node
synchronization and shared metadata traffic becoming more expensive than the
benefit from repartitioning. The current implementation is not NUMA-placement
aware: lock metadata, stats, partition maps, and data are shared across nodes.

## Notes For Presentation

- On a single NUMA node, adaptive repartitioning gives a strong result:
  `1.86x` on `shift_hotspot_point`.
- On two NUMA nodes with moderate thread count, speedups remain positive.
- With 64 threads across two NUMA nodes, cross-node synchronization dominates
  the shift hotspot case; this is a scaling limitation of the current
  implementation, not a failure to identify the hotspot.
- `moving_small_window` remains positive even across two NUMA nodes, including
  the longer divisor-20 run.
