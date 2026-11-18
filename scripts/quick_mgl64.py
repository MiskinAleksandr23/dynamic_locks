#!/usr/bin/env python3
"""Measure MGL with 64 leaves on the eight paper workloads, on 16 then 32 workers."""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from paper_experiments import CASES, ROOT

FIELDS = ("threads", "case", "original_Q", "measured_Q", "wall_s", "qps", "estimated_original_wall_s",
          "status", "verified", "hierarchy_leaves", "hierarchy_nodes", "hierarchy_bytes",
          "effective_lock_kind", "case_elapsed_s", "pilot_Q", "pilot_wall_s", "error")
CASE_TIMEOUT = 60.0


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def atomic_text(path, text):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(text, encoding="utf-8")
    temporary.replace(path)


def save_csv(path, rows):
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=FIELDS)
    writer.writeheader()
    writer.writerows(rows)
    atomic_text(path, stream.getvalue())


def command(binary, output, name, threads, operations, seed):
    scenario, _, length, points, reads, interval = CASES[name]
    ranges = scenario.endswith("_range")
    return [str(binary), "--model", "paper", "--write-body", "range",
            "--implementations", "mgl", "--mgl-leaves", "64", "--lock-kind", "mutex",
            "--scenario", scenario, "--threads", str(threads), "--repetitions", "1",
            "--operations", str(operations), "--array-size", str(1 << (26 if ranges else 20)),
            "--hot-width", str(1 << (22 if ranges else 14)), "--range-length", str(length),
            "--point-percent", str(points), "--read-percent", str(reads),
            "--bins", "1024", "--lambda", "10000", "--sample-rate", "64",
            "--rebuild-ms", str(interval), "--min-samples", "640", "--min-improvement", "0.1",
            "--trace-size", "1048576", "--seed", str(seed), "--csv", str(output)]


def read_result(path, cmd):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise ValueError("expected exactly one completed MGL64 measurement")
    row = rows[0]
    if row.get("verified") != "true" or row.get("implementation") != "mgl":
        raise ValueError("measurement is not a verified MGL64 result")
    for flag, field in (("--threads", "threads"), ("--operations", "operations"),
                        ("--scenario", "scenario"), ("--array-size", "array_size"),
                        ("--hot-width", "hot_width"), ("--range-length", "range_length"),
                        ("--read-percent", "read_percent"), ("--point-percent", "point_percent"),
                        ("--trace-size", "trace_size")):
        if row.get(field) != cmd[cmd.index(flag) + 1]:
            raise ValueError("mismatched result field: " + field)
    if (row.get("hierarchy_leaves") != "64" or row.get("hierarchy_nodes") != "127"
            or row.get("effective_lock_kind") != "atomic_mgl"):
        raise ValueError("unexpected MGL hierarchy or effective lock kind")
    if int(row.get("hierarchy_bytes", "0")) <= 0:
        raise ValueError("missing MGL hierarchy memory size")
    wall = float(row["wall_s"])
    if not math.isfinite(wall) or wall <= 0:
        raise ValueError("invalid measured wall time")
    return row


def execute(cmd, log_path, deadline, job, save):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("case exceeded 60-second pilot + measurement budget")
    started = time.monotonic()
    with log_path.open("w") as log:
        # Stay in the placement runner's process group, so its interruption
        # handling reaches us and the benchmark. The binary spawns only threads.
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        try:
            job["pid"] = proc.pid
            save()
            code = proc.wait(timeout=max(0.001, deadline - time.monotonic()))
            if code:
                raise RuntimeError(f"benchmark exited with status {code}")
        except subprocess.TimeoutExpired:
            raise TimeoutError("case exceeded 60-second pilot + measurement budget") from None
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
    return time.monotonic() - started


def worker(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = args.binary.resolve()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    rows = []
    manifest = {"status": "running", "started_utc": stamp(), "threads": args.threads,
                "binary": str(binary), "binary_sha256": digest, "target_seconds": args.target_seconds,
                "script_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                  for path in (Path(__file__).resolve(), ROOT / "scripts/paper_experiments.py")},
                "case_timeout_seconds": CASE_TIMEOUT, "seed": 97, "pilot_seed": 1_000_097,
                "method": "MGL64 only; one repeat at calibrated Q. estimated_original_wall_s = original_Q / measured qps. "
                          "Separate pilot and measured processes; full array verification and all phases. "
                          "Benchmark wall includes lock lifetime; case_elapsed_s includes pilot and process setup. "
                          "Per-phase traces repeat cyclically; a reduced Q can reduce unique queries. "
                          "Worker exits zero after all cases even if incomplete, so placement proceeds to 32t; "
                          "outer wrapper checks this manifest and returns nonzero for incomplete results.",
                "jobs": []}

    def save():
        atomic_text(output / "manifest.json", json.dumps(manifest, indent=2) + "\n")
        save_csv(output / "summary.csv", rows)

    def interrupt(_signal, _frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, interrupt)
    save()
    try:
        for name, spec in CASES.items():
            started = time.monotonic()
            deadline = started + CASE_TIMEOUT
            original = spec[1]
            pilot_q = min(original, 100_000 if spec[2] > 1 else 250_000)
            job = {"name": name, "status": "running", "started_utc": stamp(), "commands": []}
            manifest["jobs"].append(job)
            row = dict.fromkeys(FIELDS, "")
            row.update(threads=args.threads, case=name, original_Q=original,
                       status="incomplete", verified="false", pilot_Q=pilot_q)
            print(f"{args.threads}t START {name}: pilot Q={pilot_q}", flush=True)
            try:
                if hashlib.sha256(binary.read_bytes()).hexdigest() != digest:
                    raise RuntimeError("benchmark binary changed during series")
                pilot_path = output / f"{name}.pilot.csv"
                pilot_cmd = command(binary, pilot_path, name, args.threads, pilot_q, 1_000_097)
                job["commands"].append({"stage": "pilot", "argv": pilot_cmd})
                save()
                pilot_elapsed = execute(pilot_cmd, output / f"{name}.pilot.log", deadline, job, save)
                pilot = read_result(pilot_path, pilot_cmd)
                pilot_wall = float(pilot["wall_s"])
                row["pilot_wall_s"] = pilot_wall
                # Account for measured-process startup/reference-generation cost
                # using the pilot's elapsed-minus-benchmark time. A 5% margin
                # helps absorb short-pilot rate noise; the hard deadline is separate.
                setup = max(0.0, pilot_elapsed - pilot_wall)
                remaining_target = args.target_seconds - (time.monotonic() - started) - setup
                if remaining_target <= 0:
                    raise TimeoutError("pilot/setup consumed target budget; no measured result")
                measured_q = min(original, max(args.threads * 9,
                    int(pilot_q / pilot_wall * remaining_target * 0.95)))
                measured_path = output / f"{name}.measured.csv"
                measured_cmd = command(binary, measured_path, name, args.threads, measured_q, 97)
                job["commands"].append({"stage": "measured", "argv": measured_cmd})
                job["measured_Q"] = measured_q
                save()
                print(f"{args.threads}t MEASURE {name}: Q={measured_q} (original {original})", flush=True)
                execute(measured_cmd, output / f"{name}.measured.log", deadline, job, save)
                measured = read_result(measured_path, measured_cmd)
                if time.monotonic() > deadline:
                    raise TimeoutError("case exceeded 60-second total budget")
                for field in ("wall_s", "verified", "hierarchy_leaves", "hierarchy_nodes",
                              "hierarchy_bytes", "effective_lock_kind"):
                    row[field] = measured[field]
                rate = measured_q / float(measured["wall_s"])
                row.update(measured_Q=measured_q, qps=rate, estimated_original_wall_s=original / rate,
                           status="complete")
            except (OSError, ValueError, KeyError, RuntimeError, TimeoutError, csv.Error) as error:
                row["error"] = str(error)
                job["error"] = str(error)
            except KeyboardInterrupt:
                row.update(status="interrupted", error="interrupted")
                raise
            finally:
                row["case_elapsed_s"] = time.monotonic() - started
                job.update(status=row["status"], finished_utc=stamp(), elapsed_s=row["case_elapsed_s"])
                rows.append(row)
                save()
                print(f"{args.threads}t {name}: {row['status']} wall_s={row['wall_s']} "
                      f"qps={row['qps']} case_elapsed_s={row['case_elapsed_s']:.2f}", flush=True)
        manifest["status"] = "complete" if all(r["status"] == "complete" for r in rows) else "incomplete"
    except KeyboardInterrupt:
        manifest["status"] = "interrupted"
        return 130
    finally:
        manifest["finished_utc"] = stamp()
        save()
        signal.signal(signal.SIGTERM, previous)
    # Placement must run both worker counts despite case timeouts. The outer
    # wrapper, not placement's subprocess status, reports incomplete screening.
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "BENCHMARK_RESULT" /
                        ("quick-mgl64-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")))
    parser.add_argument("--binary", type=Path, default=ROOT / "build/server-release/adaptive_lock_benchmark")
    parser.add_argument("--target-seconds", type=float, default=40.0,
                        help="target whole-case time including pilot/setup, 2..45 (default 40)")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--threads", type=int, default=16, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if not math.isfinite(args.target_seconds) or not 2 <= args.target_seconds <= 45:
        parser.error("target seconds must be finite and in [2,45]")
    if not 1 <= args.threads <= 1024:
        parser.error("worker threads must be in [1,1024]")
    args.output = args.output.resolve()
    args.binary = args.binary.resolve()
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error("benchmark binary must exist and be executable: " + str(args.binary))
    if args.output.exists():
        parser.error("output directory must be new: " + str(args.output))
    if args.worker:
        return worker(args)
    cmd = [sys.executable, str(ROOT / "scripts/run_single_node.py"), "--threads", "16", "32",
           "--node", "0", "--allow-smt", "--execute", "--output", str(args.output), "--",
           sys.executable, str(Path(__file__).resolve()), "--worker", "--threads", "{threads}",
           "--output", "{output}", "--binary", str(args.binary),
           "--target-seconds", str(args.target_seconds)]
    placement = subprocess.Popen(cmd, cwd=ROOT)
    try:
        code = placement.wait()
    except KeyboardInterrupt:
        # Let placement stop its active process group and preserve interruption
        # metadata instead of killing the coordinator while its workers survive.
        if placement.poll() is None:
            placement.send_signal(signal.SIGINT)
        while True:
            try:
                placement.wait()
                break
            except KeyboardInterrupt:
                continue
        code = 130
    if code:
        # A placement failure (e.g. an output directory created by another run)
        # does not grant this process ownership of any existing summary.
        return code
    rows = []
    incomplete = False
    for threads in (16, 32):
        directory = args.output / f"{threads}t"
        try:
            manifest = json.loads((directory / "manifest.json").read_text())
            with (directory / "summary.csv").open(newline="") as stream:
                part = list(csv.DictReader(stream))
            rows.extend(part)
            incomplete |= manifest.get("status") != "complete" or len(part) != len(CASES)
            incomplete |= any(row["status"] != "complete" for row in part)
        except (OSError, ValueError, KeyError):
            incomplete = True
    if args.output.exists():
        save_csv(args.output / "summary.csv", rows)
        atomic_text(args.output / "quick_status.json", json.dumps({
            "status": "incomplete" if incomplete else "complete", "finished_utc": stamp(),
            "placement_returncode": code, "command": cmd,
            "completed_cases": sum(row["status"] == "complete" for row in rows),
            "expected_cases": 2 * len(CASES)}, indent=2) + "\n")
    print("\nMGL64 screening: wall_s and qps are measured at measured_Q.")
    for row in rows:
        print(f"{row['threads']:>2}t {row['case']:<24} {row['status']:<10} "
              f"Q={row['measured_Q']}/{row['original_Q']} wall_s={row['wall_s']} qps={row['qps']}")
    print(f"{'INCOMPLETE' if incomplete else 'COMPLETE'}: {args.output / 'summary.csv'}")
    return 1 if incomplete else 0


if __name__ == "__main__":
    raise SystemExit(main())
