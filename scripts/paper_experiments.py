#!/usr/bin/env python3
"""Sequential, reproducible paper-model measurements; no NUMA/platform machinery.

Example: python3 scripts/paper_experiments.py --binary build/paper-release/adaptive_lock_benchmark \
    --output BENCHMARK_RESULT/paper-validation --threads 8 --repetitions 3
"""
import argparse
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
# name: (generator, operations, range_length, point_percent, read_percent, interval_ms)
CASES = {
    "islands_range": ("islands_range", 96_000_000, 10_000, 0, 0, 30_000),
    "uniform": ("uniform", 5_000_000_000, 1, 0, 0, 30_000),
    "clusters_2": ("clusters_2", 2_000_000_000, 1, 0, 0, 30_000),
    "unequal_widths_range": ("unequal_widths_range", 96_000_000, 10_000, 0, 0, 30_000),
    "steady_range": ("steady_range", 64_000_000, 10_000, 0, 0, 30_000),
    "sliding_variable_range": ("sliding_variable_range", 180_000_000, 10_000, 0, 0, 10_000),
    "mixed_range": ("steady_range", 96_000_000, 1000, 50, 90, 10_000),
    "hot_with_spike": ("hot_with_spike", 2_000_000_000, 1, 0, 0, 30_000),
}
MATCH = ("scenario", "threads", "operations", "seed", "trace_size", "unique_queries",
         "array_size", "hot_width", "range_length", "sample_rate", "rebuild_interval_ms",
         "model", "bins", "lambda", "min_samples", "min_improvement", "write_body",
         "lock_kind", "read_percent", "point_percent", "read_operations", "write_operations",
         "point_operations", "range_operations")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/paper-release/adaptive_lock_benchmark")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--seed", type=int, default=97)
    parser.add_argument("--lambda", dest="penalty", type=int, default=10_000)
    parser.add_argument("--bins", type=int, default=1024)
    parser.add_argument("--cases", nargs="+", choices=list(CASES), default=list(CASES))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    binary = args.binary.resolve()
    sources = ["dynamic/paper_lock.hpp", "dynamic/paper_partitioner.hpp", "common/range_lock.hpp",
               "common/naive_lock.hpp", "benchmarks/adaptive_lock_benchmark.cpp"]
    metadata = {"status": "running", "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "platform": platform.platform(), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "source_sha256": {s: hashlib.sha256((ROOT/s).read_bytes()).hexdigest() for s in sources},
                "threads": args.threads, "repetitions": args.repetitions,
                "method": "Paper quantiles and exact CHT objective with raw integer lambda. Whole-range writes increment every element by1. All lock lifetime/cold start/statistics/rebuilding included. Same requests and fresh fixed within each repetition; stable independent seeds. Per-phase traces cyclic. Phase transitions depend on completed query counts, not wall time. Optimizer and publication timings are subsets of total wall time, never subtracted. All final arrays verified.",
                "jobs": []}
    def save():
        temporary = output / "manifest.json.tmp"
        temporary.write_text(json.dumps(metadata, indent=2) + "\n")
        temporary.replace(output / "manifest.json")
    for name in args.cases:
        scenario, count, length, points, reads, interval = CASES[name]
        ranges = scenario.endswith("_range")
        command = [str(binary), "--model", "paper", "--write-body", "range",
                   "--implementations", "fixed,balanced,dynamic_dp", "--lock-kind", "mutex",
                   "--scenario", scenario, "--threads", str(args.threads), "--repetitions", str(args.repetitions),
                   "--operations", str(count), "--array-size", str(1 << (26 if ranges else 20)),
                   "--hot-width", str(1 << (22 if ranges else 14)), "--range-length", str(length),
                   "--point-percent", str(points), "--read-percent", str(reads), "--bins", str(args.bins),
                   "--lambda", str(args.penalty), "--sample-rate", "64", "--rebuild-ms", str(interval),
                   "--min-samples", "640", "--min-improvement", "0.1", "--trace-size", "1048576",
                   "--seed", str(args.seed), "--csv", str(output / (name + ".csv"))]
        metadata["jobs"].append({"name": name, "command": command, "status": "pending"})
    save()
    for index, job in enumerate(metadata["jobs"], 1):
        if hashlib.sha256(binary.read_bytes()).hexdigest() != metadata["binary_sha256"]:
            raise SystemExit("Benchmark binary changed during series")
        job.update(status="running", started_utc=dt.datetime.now(dt.timezone.utc).isoformat())
        started = time.monotonic()
        save()
        print(f"[{index}/{len(metadata['jobs'])}] START {job['name']}", flush=True)
        with (output / (job["name"] + ".txt")).open("w") as log:
            process = subprocess.Popen(job["command"], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            job["pid"] = process.pid
            save()
            for line in process.stdout:
                log.write(line)
                log.flush()
                if "verified=true" in line or "paired_speedup=" in line: print(line.strip(), flush=True)
            code = process.wait()
        job.update(returncode=code, elapsed_s=time.monotonic()-started,
                   finished_utc=dt.datetime.now(dt.timezone.utc).isoformat())
        try:
            if code: raise ValueError(f"benchmark exit {code}")
            with (output / (job["name"] + ".csv")).open() as stream: rows = list(csv.DictReader(stream))
            if len(rows) != 3*args.repetitions: raise ValueError("incomplete repetitions")
            for rep in range(1, args.repetitions+1):
                matched = [r for r in rows if r["repetition"] == str(rep)]
                if {r["implementation"] for r in matched} != {"fixed", "balanced", "dynamic_dp"}: raise ValueError("incomplete triple")
                if len({tuple(r[k] for k in MATCH) for r in matched}) != 1: raise ValueError("unmatched inputs/settings")
            if not all(r["verified"] == "true" for r in rows): raise ValueError("unverified measurement")
            job["status"] = "complete"
        except ValueError as error:
            job.update(status="failed", error=str(error))
            metadata["status"] = "failed"
            save()
            raise
        save()
        print("COMPLETE", job["name"], flush=True)
    metadata.update(status="complete", finished_utc=dt.datetime.now(dt.timezone.utc).isoformat(),
                    elapsed_s=sum(j["elapsed_s"] for j in metadata["jobs"]))
    save()


if __name__ == "__main__":
    main()
