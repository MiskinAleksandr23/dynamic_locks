Author CA tree versus fixed AVL64 (separate Java experiment).

8 workers; 3 repetitions; JDK 21.0.8; N=1,048,576, H=65,536. Write-only: increment every existing key in the inclusive range. Cyclic trace: 65,536 stored entries across all workers. Q is total, not per worker.

| Workload | Q (M) | Length | Fixed (M queries/s) | CA (M queries/s) | CA / fixed, median [min–max] |
|---|---:|---:|---:|---:|---:|
| steady_range | 8 | 1000 | 0.302 | 0.706 | 2.33 [2.29–2.45] |
| islands_range | 8 | 1000 | 0.188 | 0.429 | 2.29 [2.21–2.43] |
| uniform_point | 100 | 1 | 8.301 | 8.390 | 1.01 [0.94–1.11] |

Throughput columns are medians; speedup is the median of matched per-seed ratios, not a ratio of medians.

steady_range samples starts uniformly within [0,H). islands_range chooses uniformly among four islands of width H/16, starting at 0, 2H/16, 8H/16, 10H/16; with these N/H settings they lie in pairs in fixed regions 0 and 2. uniform_point samples the whole key domain; its requests have length 1.

Both variants use the unchanged author's AVL implementation and SeqRWLock. Fixed has 64 equal key regions. CA may change the number and boundaries of its regions; it does not have a fixed lock budget. CA obtains all affected base locks then updates/unlocks left to right; fixed holds all affected locks through the complete update and releases in reverse order. This is a comparison of complete map implementations.

Each run uses a fresh JVM and 200,000 warmup operations on a separate disposable map. The measured CA starts with one base node. Wall time includes worker creation/join, operations, native adaptation and GC. Data population, trace/reference generation, separate-instance warmup and final validation are excluded equally. Warmup does not guarantee that JIT compilation has fully settled. Every final key is checked independently.

These numbers must not be added to the C++ array-lock table or interpreted as CA versus our Balanced/DP. The Java map storage, traversal and locking primitives differ from that experiment. This trial is write-only; native subSet returns keys, not an atomic snapshot of their values.

[Raw results](results.csv) · [Exact commands, JVM flags and source/class hashes](manifest.json) · [Node counts, GC and paired ratios](summary.json)

Regenerate: `python3 scripts/summarize_ca_tree.py BENCHMARK_RESULT/2026-11-17/ca_comparison`.
