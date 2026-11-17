# Author CA tree: separate Java experiment

This directory vendors the AVL-based **CA tree with atomic range updates** from
[Kjell Winblad's JavaRQBench](https://github.com/kjellwinblad/JavaRQBench), pinned to
commit [`df871bafbf3ea5b08e3fcda17f47e6bb3fb5040d`](https://github.com/kjellwinblad/JavaRQBench/tree/df871bafbf3ea5b08e3fcda17f47e6bb3fb5040d).
The ten Java source files under `upstream/` are unchanged. Their SHA-256 and Git
blob SHA-1 hashes, together with the upstream README and licence hashes, are in
[source.json](source.json). The runner verifies the SHA-256 hashes before each
series. Source headers specify **GPL-3.0-or-later**; see
[upstream/LICENCE](upstream/LICENCE).

The implementation is `CATreeMapSTDR`, described by Sagonas and Winblad in
[Efficient Support for Range Queries and Range Updates Using Contention Adapting
Search Trees](https://winsh.me/papers/lcpc2015.pdf). It is a CA tree, not LFCA.
The author `SeqRWLock`, contention statistics, split/join logic and thresholds
are retained; no replacement lock or retuning is applied.

## What is compared

[CATreeRangeBenchmark.java](../../benchmarks/java/CATreeRangeBenchmark.java)
compares two write-only ordered maps in the same JVM environment:

- `ca`: the unchanged author CA tree.
- `fixed_avl`: our harness baseline with 64 fixed contiguous key regions, using
  the same author `AVLTreeMapSTD`, range traversal and `SeqRWLock` write-lock
  primitive. It does not collect adaptation statistics.

Both maps are populated with every integer key `0..N-1`. Every request increments
each value in the inclusive interval `[left, right]`; a point request has
`left == right`. Native `rangeUpdate` updates existing keys only, so dense
population is essential. Each key has its own mutable value cell, avoiding a new
boxed value allocation on every increment. The update callback cannot throw.
This harness does not benchmark range reads: the upstream `subSet` returns keys,
and composing it with separate `get` calls would not provide an atomic snapshot
of values.

Both implementations acquire all affected locks before modifying values, but
their release schedules differ. CA updates and releases successive base nodes;
the remaining base nodes stay locked. `fixed_avl` holds all affected locks until
the entire update is finished and then releases them in reverse order. Both
follow two-phase locking and provide atomic updates, but this experiment does
not isolate partition placement from that release-schedule difference.

This is a **separate Java ordered-map experiment**, not a direct baseline for the
C++ dense-array benchmark. Tree traversal, representation and the JVM are part
of its cost. Report CA versus `fixed_avl` within this experiment; do not interpret
raw Java/C++ throughput ratios as speedups of their locking algorithms or merge
these rows into the C++ array results.

## Measurement and workloads

[scripts/ca_tree_experiments.py](../../scripts/ca_tree_experiments.py) launches a
fresh JVM for each measurement. JVM warmup uses a separate map. The measured CA
starts with one base node, so its adaptation is included in elapsed time.
Timing includes worker startup, all requests, adaptation and worker completion;
population, trace generation, separate-instance warmup and final verification
are excluded. GC activity during measurement is recorded. An independent oracle
checks **every final value**, not just a total checksum.

Defaults are `N=2^20` keys, hot-window width `H=2^16`, range length 1,000 and
64 fixed regions. The scenarios are:

- `uniform_point`: uniform single-key increments over all `N` keys.
- `steady_range`: uniformly selected range starts inside `[0,H-1]`.
- `islands_range`: four equally likely islands of width `H/16`, starting at
  `0`, `2H/16`, `8H/16` and `10H/16`; a range fits entirely inside its island.
  With the defaults, the islands fall in pairs into two fixed regions.

The default counts are 8,000,000 total requests for each range scenario and
100,000,000 total requests for the point scenario, divided among workers.
The default trace contains 65,536 stored requests in total and is replayed
cyclically; `unique_queries` counts stored entries, not distinct intervals.
Both implementations receive matching traces and operation counts. Repetitions
change the seed and alternate implementation order; the reported ratio is the
median of paired CA/`fixed_avl` throughput ratios.

## Reproduction

Run from the repository root with Python 3 and a **JDK 17 or newer** (`java` and
`javac` from the same installation). Sources are compiled with `--release 17`;
compilation and the full self-test suite were verified on **JDK 21.0.8**.
For example, on macOS with JDK 21 installed:

```sh
CA_JDK=$(/usr/libexec/java_home -v 21)
python3 scripts/ca_tree_experiments.py \
  --java "$CA_JDK/bin/java" --javac "$CA_JDK/bin/javac" \
  --output BENCHMARK_RESULT/ca_java_selftest --self-test-only
```

Self-tests exercise inclusive endpoints, whole-domain updates, cyclic traces,
concurrent atomic updates and per-key verification for both implementations.
To run the three scenarios with eight workers and one repetition:

```sh
python3 scripts/ca_tree_experiments.py \
  --java "$CA_JDK/bin/java" --javac "$CA_JDK/bin/javac" \
  --output BENCHMARK_RESULT/ca_java_8t \
  --threads 8 --repetitions 1 \
  --cases steady_range islands_range uniform_point
```

Use `--range-operations`, `--point-operations`, `--warmup-operations`, `--keys`,
`--hot-width`, `--length` and `--trace-size` to change the workload. An output
directory must be new. Each series saves `manifest.json`, copied source files,
compiler/self-test logs, per-measurement logs and CSV files, and combined
`results.csv`. The manifest records exact commands, seeds, versions and hashes.
The fixed JVM configuration uses G1, a 512 MiB initial / 2 GiB maximum heap,
`ActiveProcessorCount` equal to the worker count and `-XX:-UseNUMA`.

On the Linux server, put the chosen JDK on `PATH` and install `lscpu` and
`numactl`. The generic
[run_single_node.py](../../scripts/run_single_node.py) wrapper first checks the
actual topology. This command only prints the plan:

```sh
python3 scripts/run_single_node.py \
  --threads 32 64 --node auto --output BENCHMARK_RESULT/ca_java_server -- \
  python3 scripts/ca_tree_experiments.py \
    --threads '{threads}' --output '{output}' --repetitions 1 \
    --cases steady_range islands_range uniform_point
```

Add `--execute` before `--` to run the 32- and 64-worker series sequentially.
Both use one selected NUMA node, one logical CPU per physical core and memory
bound to that node. If the node does not contain 64 permitted physical cores,
the wrapper fails instead of using SMT siblings or crossing nodes. All JVM
threads inherit the process CPU set; workers are not individually pinned, and
GC/compiler threads share that set. The wrapper records placement in
`placement.json`; measurements go into `32t/` and `64t/`.
