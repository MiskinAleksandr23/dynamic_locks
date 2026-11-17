Throughput relative to fixed (higher is better).

8 worker threads; K=64; B=1024; lambda=10000; sampling 1/64; three repetitions; median paired throughput ratios. Writes increment every covered element. Total time includes cold start, statistics, rebuilding and publication.

| Workload | Q (M) | Bal. | DP |
|---|---:|---:|---:|
| uniform | 5000 | 0.89 | 0.91 |
| islands_range | 96 | 1.98 | 2.00 |
| clusters_2 | 2000 | 1.59 | 1.58 |
| unequal_widths_range | 96 | 1.83 | 1.90 |
| steady_range | 64 | 1.68 | 1.68 |
| sliding_variable_range | 180 | 1.78 | 1.72 |
| mixed_range | 96 | 1.85 | 1.88 |
| hot_with_spike | 2000 | 0.76 | 0.78 |

Intervals: 30 seconds, except sliding_variable_range and mixed_range (10 seconds). Mixed range: 50% point requests, 90% read-only requests, remaining ranges length1000. Other interval workloads use length10000 (sliding variable:1..10000). These are new paper-model results, not replacements for historical tables.

[Raw summary](summary.csv) · [Commands and hashes](manifest.json) · [Method and build instructions](../../../README.md)
