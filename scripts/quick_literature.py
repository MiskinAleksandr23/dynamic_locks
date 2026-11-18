#!/usr/bin/env python3
"""One short, matched comparison on xeon5. Run each case as a separate command."""
import argparse
import csv
import datetime
import json
import math
from pathlib import Path
import subprocess
import sys
import time

from paper_experiments import MATCH
from run_single_node import digest

ROOT = Path(__file__).resolve().parents[1]
POLICIES = ("fixed", "balanced", "dynamic_dp", "mgl64", "mgl1024", "srl4", "srl64", "srl256")
# generator, N, hot width, range length, point %, read %, calibration Q
CASES = {
    "steady_range": ("steady_range", 2**26, 2**22, 10_000, 0, 0, 120_000),
    "islands_range": ("islands_range", 2**26, 2**22, 10_000, 0, 0, 120_000),
    "uniform": ("uniform", 2**20, 2**14, 1, 0, 0, 1_000_000),
    "mixed_range": ("steady_range", 2**26, 2**22, 1000, 50, 90, 300_000),
}


def read_cohort(path, operations):
    with path.open() as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != len(POLICIES) or {r["implementation"] for r in rows} != set(POLICIES):
        raise ValueError(f"Incomplete comparison: {path}")
    if any(r["verified"] != "true" or int(r["operations"]) != operations for r in rows):
        raise ValueError(f"Invalid measurement in {path}")
    if len({tuple(r[k] for k in MATCH) for r in rows}) != 1:
        raise ValueError(f"Input traces or parameters differ within {path}")
    return rows


def measure(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    scenario, n, hot, length, points, reads, pilot_q = CASES[args.case]
    command = [str(args.binary), "--scenario", scenario, "--model", "paper", "--write-body", "range",
               "--implementations", ",".join(POLICIES), "--threads", str(args.threads), "--repetitions", "1",
               "--array-size", str(n), "--hot-width", str(hot), "--range-length", str(length),
               "--point-percent", str(points), "--read-percent", str(reads),
               "--lock-kind", "rwlock" if reads else "mutex", "--rebuild-ms", "5000",
               "--bins", "1024", "--sample-rate", "64", "--min-samples", "640",
               "--min-improvement", "0.1", "--lambda", "10000", "--trace-size", "65536"]
    start = time.monotonic()
    hard_limit = args.target_seconds + 60
    meta = {"case": args.case, "threads": args.threads, "binary_sha256": digest(args.binary),
            "target_s": args.target_seconds, "hard_limit_s": hard_limit, "status": "running", "jobs": []}

    def save():
        tmp = out / "manifest.tmp"
        tmp.write_text(json.dumps(meta, indent=2) + "\n")
        tmp.replace(out / "manifest.json")

    def run(label, count, seed):
        csv_path = out / f"{label}.csv"
        cmd = [*command, "--operations", str(count), "--seed", str(seed), "--csv", str(csv_path)]
        meta["jobs"].append({"phase": label, "command": cmd, "operations": count})
        save()
        print(f"{label}: {count:,} requests per implementation", flush=True)
        remaining = start + hard_limit - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(cmd, hard_limit)
        with (out / f"{label}.log").open("w") as log:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=remaining)
        return read_cohort(csv_path, count)

    try:
        probe = run("calibration", pilot_q, 197)
        elapsed = time.monotonic() - start
        timed = sum(float(r["wall_s"]) for r in probe)
        if not math.isfinite(timed) or timed <= 0:
            raise ValueError("Invalid calibration time")
        # Reserve another setup and verification pass. Use one Q for all policies.
        work_seconds = args.target_seconds - elapsed - max(0, elapsed - timed)
        quantum = args.threads * 4
        count = max(1, int(pilot_q * max(0, work_seconds) / timed) // quantum) * quantum
        if work_seconds <= 0:
            raise ValueError("Calibration consumed the target budget")
        rows = run("results", count, 97)
        base = float(next(r["wall_s"] for r in rows if r["implementation"] == "fixed"))
        with (out / "summary.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["implementation", "total_requests", "wall_s", "queries_per_second", "vs_fixed", "rebuilds"])
            for r in rows:
                wall = float(r["wall_s"])
                writer.writerow([r["implementation"], count, wall, count / wall, base / wall, r["rebuilds"]])
        meta["status"] = "complete"
        print((out / "summary.csv").read_text(), end="", flush=True)
    except subprocess.TimeoutExpired:
        meta["status"] = "timeout"
        print(f"Time limit reached. Completed raw rows are in {out}.", file=sys.stderr)
        return 124
    except BaseException as error:
        meta.update(status="failed", error=str(error))
        raise
    finally:
        meta["elapsed_s"] = time.monotonic() - start
        save()
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("case", choices=list(CASES))
    p.add_argument("--threads", type=int, choices=[16, 32], default=16)
    p.add_argument("--target-seconds", type=int, default=150, help="calibration and comparison together (default 150)")
    p.add_argument("--binary", type=Path, default=ROOT / "build/literature-quick/adaptive_lock_benchmark")
    p.add_argument("--output", type=Path, help="new output directory (default: timestamped)")
    p.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = p.parse_args()
    if not 15 <= args.target_seconds <= 180:
        p.error("target must be 15..180 seconds")
    args.binary = args.binary.resolve()
    if not args.binary.is_file():
        p.error(f"build the benchmark first: {args.binary}")
    if args.worker:
        return measure(args)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = (args.output or ROOT / "BENCHMARK_RESULT" / f"literature-{args.case}-{args.threads}t-{stamp}").resolve()
    worker_out = out / f"{args.threads}t"
    print(f"Measurements: {worker_out}/results.csv\nProgress: {out}/{args.threads}t.console.log", flush=True)
    cmd = [sys.executable, str(ROOT / "scripts/run_single_node.py"), "--execute", "--threads", str(args.threads),
           "--node", "0", "--output", str(out)]
    if args.threads == 32:
        cmd.append("--allow-smt")
    cmd.extend(["--", sys.executable, str(Path(__file__).resolve()), args.case, "--worker", "--threads", "{threads}",
                "--binary", str(args.binary), "--output", "{output}", "--target-seconds", str(args.target_seconds)])
    code = subprocess.call(cmd)
    if code == 0:
        print((worker_out / "summary.csv").read_text(), end="")
    return code


if __name__ == "__main__":
    sys.exit(main())
