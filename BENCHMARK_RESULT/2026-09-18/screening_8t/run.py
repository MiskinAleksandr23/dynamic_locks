#!/usr/bin/env python3
"""One sequential, matched repetition of every retained scenario on 8 workers."""
import csv
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
CASES = [
    ("islands_range", "2026-09-15/islands_range.csv", 96_000_000),
    ("gradient_range", "2026-09-15/gradient_range.csv", 96_000_000),
    ("unequal", "2026-09-14/rare_unequal.csv", 2_000_000_000),
    ("unequal_widths_range", "2026-09-15/unequal_widths_range.csv", 96_000_000),
    ("clusters_2", "2026-09-14/rare_clusters_2.csv", 2_000_000_000),
    ("steady_range", "2026-09-14/rare_large_range.csv", 64_000_000),
    ("moving_variable_range", "2026-09-15/moving_variable_range.csv", 160_000_000),
    ("exchange_range", "2026-09-15/exchange_range.csv", 96_000_000),
    ("shift", "2026-09-14/rare_shift.csv", 2_000_000_000),
    ("clusters_8", "2026-09-14/rare_clusters_8.csv", 2_000_000_000),
    ("clusters_2_range", "2026-09-14/rare_clusters_2_range.csv", 64_000_000),
    ("clusters_4_range", "2026-09-14/rare_clusters_4_range.csv", 64_000_000),
    ("uniform", "2026-09-14/uniform_overhead.csv", 5_000_000_000),
    ("hot_with_spike", "2026-09-14/rare_spike.csv", 2_000_000_000),
]


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def write_json(name, value):
    temp = OUT / (name + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temp.replace(OUT / name)


def main():
    if (OUT / "runs.json").exists():
        raise SystemExit("Existing run manifest: refusing to overwrite measurements")
    sources = ["dynamic/dynamic_lock.hpp", "dynamic/partitioner.hpp",
               "common/range_lock.hpp", "common/naive_lock.hpp",
               "benchmarks/adaptive_lock_benchmark.cpp", "build/adaptive_lock_benchmark"]
    metadata = {
        "started_utc": stamp(), "platform": platform.platform(),
        "machine": platform.machine(), "logical_cpus": os.cpu_count(),
        "workers": 8, "sequential": True, "repetitions": 1,
        "build_validation": "cmake --build build -j 6; ctest --test-dir build --output-on-failure: 6/6 passed",
        "source_sha256": {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in sources},
        "method": [
            "Fixed operation counts, identical input and count for all three implementations.",
            "Wall/CPU time include construction, cold start, sampling, background rebuilding and destruction.",
            "30 s is the background check interval, not a mandatory repartition.",
            "No concurrent benchmark processes; 8 workers plus an adaptive background thread; no CPU affinity reservation.",
            "All 14 retained scenarios; original geometry, phase transitions and stable seed/order identities.",
            "Uniform extended from 32M to 5B operations to cover multiple 30 s checks.",
            "Moving variable range reduced from 192M to 160M operations to leave margin below 3 minutes per implementation.",
            "Other operation counts match the previous dedicated measurements.",
            "One repetition is for screening; no confidence interval or claim of statistical significance.",
        ],
    }
    runs = []
    for scenario, source, operations in CASES:
        with (ROOT / "BENCHMARK_RESULT" / source).open() as stream:
            prior = next(csv.DictReader(stream))
        assert prior["scenario"] == scenario
        output_csv = OUT / (scenario + ".csv")
        if output_csv.exists():
            raise SystemExit(f"Refusing to overwrite {output_csv}")
        command = [str(ROOT / "build/adaptive_lock_benchmark"),
                   "--scenario", scenario, "--threads", "8", "--repetitions", "1",
                   "--operations", str(operations), "--array-size", prior["array_size"],
                   "--hot-width", prior["hot_width"], "--range-length", prior["range_length"],
                   "--sample-rate", "64", "--rebuild-ms", "30000", "--seed", "1",
                   "--trace-size", "1048576", "--csv", str(output_csv)]
        runs.append({"scenario": scenario, "previous_csv": source,
                     "previous_operations": int(prior["operations"]),
                     "command": command, "status": "pending"})
    write_json("metadata.json", metadata)
    write_json("runs.json", runs)
    for index, run in enumerate(runs, 1):
        run.update(status="running", started_utc=stamp(), start_epoch=time.time())
        write_json("runs.json", runs)
        print(f"[{index}/{len(runs)}] START {run['scenario']} {run['started_utc']}", flush=True)
        with (OUT / (run["scenario"] + ".txt")).open("w") as log:
            process = subprocess.run(run["command"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        run.update(returncode=process.returncode, finished_utc=stamp(),
                   elapsed_s=time.time() - run["start_epoch"])
        if process.returncode == 0:
            with (OUT / (run["scenario"] + ".csv")).open() as stream:
                rows = list(csv.DictReader(stream))
            valid = (len(rows) == 3 and {r["implementation"] for r in rows} ==
                     {"fixed", "balanced", "dynamic_dp"} and
                     all(r["verified"] == "true" and r["threads"] == "8" for r in rows))
            run["status"] = "complete" if valid else "invalid_output"
        else:
            run["status"] = "failed"
        write_json("runs.json", runs)
        print(f"[{index}/{len(runs)}] {run['status'].upper()} {run['scenario']} {run['elapsed_s']:.1f}s", flush=True)
        if run["status"] != "complete":
            raise SystemExit(f"Stopped after unsuccessful run: {run['scenario']}")
    metadata["finished_utc"] = stamp()
    write_json("metadata.json", metadata)


if __name__ == "__main__":
    main()
