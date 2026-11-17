#!/usr/bin/env python3
"""Summarize a complete same-JVM CA/fixed AVL series; never mix with C++ results."""
import argparse
import csv
import json
from pathlib import Path
import statistics

from ca_tree_experiments import MATCH


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    out = args.directory
    manifest = json.loads((out / "manifest.json").read_text())
    if manifest["status"] != "complete" or manifest.get("self_test") != "passed":
        raise SystemExit("Series incomplete or self-test not passed")
    with (out / "results.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    settings = manifest["settings"]
    reps = settings["repetitions"]
    if len(rows) != len(settings["cases"]) * reps * 2:
        raise SystemExit("Incomplete results")
    table = []
    for case in settings["cases"]:
        ratios = []
        selected = [r for r in rows if r["scenario"] == case]
        for rep in range(1, reps + 1):
            pair = [r for r in selected if int(r["repetition"]) == rep]
            if len(pair) != 2 or {r["implementation"] for r in pair} != {"ca", "fixed_avl"}:
                raise SystemExit("Missing/duplicate pair")
            if any(r["verified"] != "true" for r in pair):
                raise SystemExit("Unverified result")
            if len({tuple(r[k] for k in MATCH) for r in pair}) != 1:
                raise SystemExit("Unmatched pair")
            by_impl = {r["implementation"]: r for r in pair}
            if by_impl["ca"]["route_nodes_before"] != "0":
                raise SystemExit("CA already adapted before timing")
            ratios.append(float(by_impl["ca"]["qps"]) / float(by_impl["fixed_avl"]["qps"]))
        entry = {"scenario": case, "operations": int(selected[0]["operations"]),
                 "length": int(selected[0]["range_length"]),
                 "ca_over_fixed": statistics.median(ratios), "paired_ratios": ratios}
        for impl in ("fixed_avl", "ca"):
            subset = [r for r in selected if r["implementation"] == impl]
            entry[impl] = {"median_wall_s": statistics.median(float(r["wall_s"]) for r in subset),
                           "median_qps": statistics.median(float(r["qps"]) for r in subset),
                           "gc_ms": [int(r["gc_ms"]) for r in subset],
                           "base_nodes_after": ([int(r["route_nodes_after"]) + 1 for r in subset]
                                                if impl == "ca" else [int(r["fixed_regions"]) for r in subset])}
        table.append(entry)
    (out / "summary.json").write_text(json.dumps(table, indent=2) + "\n")
    text = ["Author CA tree versus fixed AVL64 (separate Java experiment).", "",
            f"{settings['threads']} workers; {settings['repetitions']} repetitions; JDK {rows[0]['java_version']}; "
            f"N={settings['keys']:,}, H={settings['hot_width']:,}. Write-only: increment every existing key in the inclusive range. "
            f"Cyclic trace: {settings['trace_size']:,} stored entries across all workers. Q is total, not per worker.", "",
            "| Workload | Q (M) | Length | Fixed (M queries/s) | CA (M queries/s) | CA / fixed, median [min–max] |",
            "|---|---:|---:|---:|---:|---:|"]
    for row in table:
        values = row["paired_ratios"]
        text.append(f"| {row['scenario']} | {row['operations']/1e6:g} | {row['length']} | "
                    f"{row['fixed_avl']['median_qps']/1e6:.3f} | {row['ca']['median_qps']/1e6:.3f} | "
                    f"{row['ca_over_fixed']:.2f} [{min(values):.2f}–{max(values):.2f}] |")
    text.extend(["", "Throughput columns are medians; speedup is the median of matched per-seed ratios, not a ratio of medians.", "",
                 "steady_range samples starts uniformly within [0,H). islands_range chooses uniformly among four islands "
                 "of width H/16, starting at 0, 2H/16, 8H/16, 10H/16; with these N/H settings they lie in pairs in fixed regions 0 and 2. "
                 "uniform_point samples the whole key domain; its requests have length 1.", "",
                 "Both variants use the unchanged author's AVL implementation and SeqRWLock. Fixed has 64 equal key regions. "
                 "CA may change the number and boundaries of its regions; it does not have a fixed lock budget. "
                 "CA obtains all affected base locks then updates/unlocks left to right; fixed holds all affected locks "
                 "through the complete update and releases in reverse order. This is a comparison of complete map implementations.", "",
                 f"Each run uses a fresh JVM and {settings['warmup_operations']:,} warmup operations on a separate disposable map. "
                 "The measured CA starts with one base node. Wall time includes worker creation/join, operations, native adaptation and GC. "
                 "Data population, trace/reference generation, separate-instance warmup and final validation are excluded equally. "
                 "Warmup does not guarantee that JIT compilation has fully settled. Every final key is checked independently.", "",
                 "These numbers must not be added to the C++ array-lock table or interpreted as CA versus our Balanced/DP. "
                 "The Java map storage, traversal and locking primitives differ from that experiment. This trial is write-only; "
                 "native subSet returns keys, not an atomic snapshot of their values.", "",
                 "[Raw results](results.csv) · [Exact commands, JVM flags and source/class hashes](manifest.json) · "
                 "[Node counts, GC and paired ratios](summary.json)", "",
                 f"Regenerate: `python3 scripts/summarize_ca_tree.py {out}`.", ""])
    (out / "TABLE.md").write_text("\n".join(text))
    print("\n".join(text[:len(table) + 6]))


if __name__ == "__main__":
    main()
