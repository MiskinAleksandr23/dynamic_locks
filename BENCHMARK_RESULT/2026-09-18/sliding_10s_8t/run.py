#!/usr/bin/env python3
"""One matched repetition of eight overlapping window shifts, 10 s rebuild checks."""
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent


def save(metadata):
    (OUT / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")


def main():
    if (OUT / "metadata.json").exists():
        raise SystemExit("Refusing to overwrite existing experiment")
    sources = ["dynamic/dynamic_lock.hpp", "dynamic/partitioner.hpp", "common/range_lock.hpp",
               "common/naive_lock.hpp", "benchmarks/adaptive_lock_benchmark.cpp",
               "tests/benchmark_workload_test.cpp", "build/adaptive_lock_benchmark"]
    hashes = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in sources}
    old = json.loads((OUT.parent / "scaling_4_6_8_10/metadata.json").read_text())["source_sha256"]
    assert all(hashes[p] == old[p] for p in sources[:4]), "Lock algorithms changed"
    command = [str(ROOT / "build/adaptive_lock_benchmark"),
               "--scenario", "sliding_variable_range", "--threads", "8",
               "--array-size", "67108864", "--hot-width", "4194304",
               "--range-length", "10000", "--operations", "180000000",
               "--trace-size", "1048576", "--sample-rate", "64",
               "--rebuild-ms", "10000", "--repetitions", "1", "--seed", "1",
               "--csv", str(OUT / "results.csv")]
    metadata = {
        "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(), "status": "running",
        "command": command, "source_sha256": hashes, "lock_algorithms_unchanged": True,
        "validation": "Release build and 6/6 CTest passed, including all nine physical windows, variable lengths, invalid bounds and empty-phase rejection; all-scenario cyclic smoke includes the new scenario.",
        "compiler": "Homebrew Clang 22.1.0, Release -O3 -DNDEBUG -std=c++20",
        "geometry": {"residences": 9, "moves": 8, "window_elements": 4194304,
                     "step_elements": 1048576, "adjacent_overlap": 0.75,
                     "operations_per_residence_total": 20000000},
        "method": [
            "8 workers, 64 locks, random range length 1..10000, uniformly random valid start in the current window.",
            "Window starts at phase * (hot_width / 4); all workers synchronize between phases.",
            "Moves are driven by completed operations, not a wall-clock timer; each implementation has different residence durations.",
            "Background rebuild check every 10 seconds; no forced rebuild at a window shift.",
            "All work from lock construction to destruction, including cold start, statistics and rebuilding, is timed.",
            "One matched triple, identical inputs for all implementations; execution order DP, fixed, balanced.",
            "A stored per-worker trace repeats cyclically, with total budget 1048576 queries per phase.",
            "Full final array verified after every run; input preparation, array initialization and verification excluded equally.",
        ],
    }
    save(metadata)
    start = time.monotonic()
    with (OUT / "results.txt").open("w") as log:
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        metadata["pid"] = process.pid
        save(metadata)
        for line in process.stdout:
            log.write(line)
            log.flush()
            print(line, end="", flush=True)
        code = process.wait()
    metadata.update(returncode=code, elapsed_s=time.monotonic()-start,
                    finished_utc=dt.datetime.now(dt.timezone.utc).isoformat(), status="failed")
    if code == 0:
        with (OUT / "results.csv").open() as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == 3 and all(r["verified"] == "true" and len(r["phase_wall_s"].split(";")) == 9 for r in rows)
        assert {(r["repetition"], r["implementation"]) for r in rows} == {
            ("1", i) for i in ("fixed", "balanced", "dynamic_dp")}
        metadata.update(status="complete", verified_runs=3)
    save(metadata)
    raise SystemExit(code)


if __name__ == "__main__":
    main()
