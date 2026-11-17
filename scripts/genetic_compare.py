#!/usr/bin/env python3
"""Sequential matched runs of genetic and existing implementations.

quick preserves workload geometry but shortens runs and the rebuild interval.
full uses the operation counts and 30-second interval from BENCHMARK_SHORT.md.
"""

import argparse
import csv
import datetime
import hashlib
import json
import platform
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "uniform": 5_000_000_000,
    "clusters_2": 2_000_000_000,
    "clusters_8": 2_000_000_000,
    "unequal": 2_000_000_000,
    "shift": 2_000_000_000,
    "hot_with_spike": 2_000_000_000,
    "steady_range": 64_000_000,
    "clusters_2_range": 64_000_000,
    "clusters_4_range": 64_000_000,
    "islands_range": 96_000_000,
    "gradient_range": 96_000_000,
    "unequal_widths_range": 96_000_000,
    "exchange_range": 96_000_000,
    "moving_variable_range": 160_000_000,
    "sliding_variable_range": 180_000_000,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path,
                        default=ROOT / "build/paper-release/adaptive_lock_benchmark")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", choices=["quick", "full"], default="quick")
    parser.add_argument("--scenarios", nargs="+", choices=list(CASES), default=list(CASES))
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--rebuild-ms", type=int,
                        help="override the profile's rebuild interval")
    parser.add_argument("--implementations", nargs="+",
                        choices=["fixed", "balanced", "dynamic_dp", "genetic"],
                        default=["fixed", "balanced", "dynamic_dp", "genetic"])
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    quick = args.profile == "quick"
    repetitions = 3 if quick else 1
    rebuild_ms = args.rebuild_ms if args.rebuild_ms is not None else (100 if quick else 30000)
    sources = [binary, ROOT / "genetic/histogram_partitioner.hpp",
               ROOT / "dynamic/dynamic_lock.hpp", ROOT / "dynamic/partitioner.hpp",
               ROOT / "common/range_lock.hpp", ROOT / "common/naive_lock.hpp",
               ROOT / "benchmarks/adaptive_lock_benchmark.cpp"]
    metadata = {"profile": args.profile, "platform": platform.platform(),
                "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "threads": args.threads, "implementations": args.implementations,
                "machine": platform.machine(), "sequential": True,
                "sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                "runs": []}

    def save():
        temporary = args.output / "manifest.json.tmp"
        temporary.write_text(json.dumps(metadata, indent=2) + "\n")
        temporary.replace(args.output / "manifest.json")

    save()
    for scenario in args.scenarios:
        ranges = scenario.endswith("range")
        operations = (1_000_000 if ranges else 20_000_000) if quick else CASES[scenario]
        csv_path = (args.output / f"{scenario}.csv").resolve()
        command = [str(binary), "--model", "legacy", "--write-body", "single",
                   "--implementations", ",".join(args.implementations),
                   "--scenario", scenario, "--threads", str(args.threads),
                   "--operations", str(operations), "--repetitions", str(repetitions),
                   "--array-size", str(1 << (26 if ranges else 20)),
                   "--hot-width", str(1 << (22 if ranges else 14)),
                   "--range-length", "10000" if ranges else "1",
                   "--sample-rate", "64", "--rebuild-ms", str(rebuild_ms),
                   "--trace-size", "1048576", "--seed", "1", "--csv", str(csv_path)]
        run = {"scenario": scenario, "command": command, "status": "running",
               "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()}
        metadata["runs"].append(run)
        save()
        print(f"START {scenario}", flush=True)
        with (args.output / f"{scenario}.txt").open("w") as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        run["returncode"] = result.returncode
        run["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        run["status"] = "failed"
        if result.returncode == 0:
            with csv_path.open() as stream:
                rows = list(csv.DictReader(stream))
            expected = {(str(rep), impl) for rep in range(1, repetitions + 1)
                        for impl in args.implementations}
            if (len(rows) == len(expected) and
                {(row["repetition"], row["implementation"]) for row in rows} == expected and
                all(row["verified"] == "true" for row in rows)):
                run["status"] = "complete"
        save()
        print(f"{run['status'].upper()} {scenario}", flush=True)
        if run["status"] != "complete":
            raise SystemExit(f"Failed: see {args.output / (scenario + '.txt')}")
    metadata["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()


if __name__ == "__main__":
    main()
