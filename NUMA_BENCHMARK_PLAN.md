# NUMA Benchmark Plan

Goal: show adaptive locks on a large NUMA server without turning the benchmark
into a pure scheduler/contention artifact. Use the honest `total_s`/`total_x`
metric from `adaptive_lock_benchmark`; do not use old `body_s`/`body_x`
results.

## Why The Old NUMA Run Was Bad

The previous `numa_div4_results.txt` run used 32 threads with small hot windows
and many scenarios where all threads could hit the same few cache lines/locks.
The aggregate `lock_sum_s` was much larger than wall time, which means most CPU
time was spent waiting. That measures oversubscription/NUMA contention more than
the partitioning algorithm.

For NUMA, prefer scenarios where each thread group owns a stable hot region:

- enough hot blocks per thread group;
- rare rebuilds compared with total request count;
- no random uniform traffic as the main result;
- no short 10-second total runs where rebuild overhead dominates;
- explicit CPU and memory placement for reproducibility.

## Scenarios To Use

Primary presentation scenarios:

| Scenario | Why |
|---|---|
| `shift_hotspot_point` | Simple hotspot shift; shows adaptation on a single moving target. |
| `clustered_4_thread_groups` | Threads are split evenly across 4 windows and striped inside each window; this avoids artificial all-thread contention. |
| `clustered_4_thread_groups_contended` | Same ownership pattern, but randomized inside each assigned window; useful as a more stressful variant. |
| `moving_small_window` | Online moving window with long phases; shows adaptation over time. |

Optional negative controls:

| Scenario | Why |
|---|---|
| `random_uniform_point` | Should not be optimized; include only to show no claim on uniform random traffic. |
| `random_uniform_ranges` | Also not a target case; useful only as a sanity check. |

Avoid as main NUMA results:

- `clustered_2_hot_windows`: too few regions for many threads.
- `clustered_4_hot_windows`: all threads can hit all windows, so it can become a scheduler/contention benchmark.
- random range scenarios as headline results.

## Server Topology First

Run once:

```bash
lscpu
numactl --hardware
```

Pick CPU lists from one node and two nodes. Do not start with all 128 cores.
For presentation, 16 or 32 threads is usually cleaner than 128. If 64/128 is
needed, use thread-group scenarios only.

## Build

```bash
./scripts/numa_build.sh
```

The scripts below save outputs under `BENCHMARK_RESULT/numa/<timestamp>_*` and
write a compact `summary.md` after each batch.

## Common Environment

Use these defaults for final NUMA runs:

```bash
export DYNAMIC_LOCK_RUN_GENETIC=1
export DYNAMIC_LOCK_REBUILD_INTERVAL_MS=10000
export DYNAMIC_LOCK_REBUILD_THRESHOLD=2.0
export DYNAMIC_LOCK_STATS_SAMPLE_RATE=64
export DYNAMIC_LOCK_REBUILD_MIN_GAIN=1.0
export DYNAMIC_LOCK_REBUILD_MIN_SKEW=4.0
export DYNAMIC_LOCK_GENETIC_TRAINING_BATCH=100000
export DYNAMIC_LOCK_GENETIC_TRAINING_SAMPLE_RATE=64
export DYNAMIC_LOCK_GENETIC_PROBE_GAP=1000000
export DYNAMIC_LOCK_GENETIC_MIN_SKEW=4.0
```

Use `DYNAMIC_LOCK_QUERY_DIVISOR=20` for smoke checks and `4` or `2` for final
numbers. Smaller divisor means more requests and more stable numbers.

## Recommended Runs

### 1. Smoke On One NUMA Node

Use 16 threads and a small run to verify that binaries and output are correct:

```bash
NODE=0 THREADS=16 ./scripts/numa_smoke.sh
```

Check that output has `total_s`, `total_x`, and rebuild/train counts.

### 2. Final One-Node Results

This is the cleanest baseline: no cross-node memory traffic.

```bash
NODE=0 THREADS=32 NUMA_PROFILE=balanced ./scripts/numa_one_node.sh
```

### 3. Final Two-Node Results

This shows NUMA behavior without jumping straight to the full 128-core case.
Replace `0,1` with the actual nodes you want.

```bash
NODES=0,1 THREADS=64 NUMA_PROFILE=balanced ./scripts/numa_two_node.sh
```

Use interleaved memory for the two-node run. `--membind=0,1` can still create
first-touch artifacts depending on initialization; interleave is usually more
presentation-friendly.

### 4. Optional Full-Machine Run

Only run this if 64-thread results are good. Use thread-group scenarios only:

```bash
THREADS=128 NUMA_PROFILE=quick ./scripts/numa_all_nodes_optional.sh
```

Profiles:

| Profile | Use | Divisors |
|---|---|---|
| `smoke` | Sanity check | grouped `80`, moving `120` |
| `quick` | Faster exploratory run | grouped/shift `4`, moving `40` |
| `balanced` | Recommended default | grouped/shift `1`, moving `20` |
| `final` | Longer run if previous results look good | grouped/shift `1`, moving `10` |

All batch scripts default to `NUMA_TIMEOUT_SECONDS=300` except smoke, which uses
`180`. Override it if a run is clearly too short or too long.

If `lock_sum_s` is again orders of magnitude larger than wall time, do not use
the full-machine run as the main result. It is then mostly showing lock
convoys/scheduler effects.

## What To Report

For each scenario report:

- `total_x` for dynamic and genetic over naive;
- `total_s`, not `measured_s`, as the main time;
- `rebuild/train` count;
- `avg_lock_us` and `lock_sum_s` as contention diagnostics;
- CPU placement: one node, two nodes, or all nodes;
- thread count and query divisor.

A good presentation table should have columns:

| Placement | Threads | Scenario | Naive total_s | Dynamic total_x | Genetic total_x | Dynamic rebuilds | Genetic trains |
|---|---:|---|---:|---:|---:|---:|---:|

## If We Add One More Scenario

The most useful future code change is a churn variant with thread ownership:
`clustered_churn_4_thread_groups` or `clustered_churn_8_thread_groups`.
It should use the same idea as `clustered_4_thread_groups`: split threads evenly
across active windows and stripe them inside each assigned window. That would
make churn less pathological on 64/128 threads and closer to a real sharded
service.
