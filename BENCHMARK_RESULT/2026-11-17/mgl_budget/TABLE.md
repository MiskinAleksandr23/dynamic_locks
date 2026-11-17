Throughput relative to fixed (higher is better).

8 workers; 24M total requests per implementation per repetition; N=2²⁶, L=10 000; whole-range writes only. Paper policies: K=64, B=1024, λ=10000, sampling 1/64, rebuild check every 10 s. Values are medians of three ratios paired by seed/repetition; cold start and rebuilding are included.

| Workload | Balanced / fixed | DP / fixed | MGL 1024 leaves / fixed | MGL 64 leaves / fixed | MGL 32 leaves / fixed |
|---|---:|---:|---:|---:|---:|
| steady_range | 1.76 | 1.67 | 2.02 | 1.34 | 1.03 |
| islands_range | 1.86 | 1.87 | 2.67 | 1.41 | 1.41 |

Direct comparisons: **our policy / MGL throughput** (>1 means our policy is faster).

| Workload | MGL leaves | Node states | Balanced / MGL | DP / MGL |
|---|---:|---:|---:|---:|
| steady_range | 1024 | 2047 | 0.81 | 0.83 |
| steady_range | 64 | 127 | 1.31 | 1.24 |
| steady_range | 32 | 63 | 1.71 | 1.64 |
| islands_range | 1024 | 2047 | 0.70 | 0.70 |
| islands_range | 64 | 127 | 1.32 | 1.33 |
| islands_range | 32 | 63 | 1.33 | 1.32 |

MGL hierarchy sizes: **1024 leaves / 2047 states / 262016 bytes**; **64 leaves / 127 states / 16256 bytes**; **32 leaves / 63 states / 8064 bytes**. These bytes describe allocated padded hierarchy nodes only. A node contains an atomic mode state, not a std::mutex; the adaptive policies also store statistics and mapping data. Similar numbers of states are not evidence of equal total memory or equal primitive cost.

The 64- and 32-leaf measurements were collected in later sequential supplemental runs. Their inputs, seeds and repetition numbers match the saved fixed/Balanced/DP runs, but the reference policies were not rerun in those cohorts. Differences may include machine-state drift. All 12 new final-array checks and 24 reference checks used here passed; no incomplete data are included.

[Detailed ratios and observed ranges](summary.json) · [Supplemental commands](manifest.json) · [Original comparison](../mgl_comparison/TABLE.md)

Reproduce this summary: `python3 BENCHMARK_RESULT/2026-11-17/mgl_budget/summarize.py`.
