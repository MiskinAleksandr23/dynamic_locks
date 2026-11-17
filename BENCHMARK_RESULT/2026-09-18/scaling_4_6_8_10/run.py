#!/usr/bin/env python3
"""Run the unchanged 8-worker workload on 4, 6, 10 workers, sequentially.

Results on 8 workers are reused from ../screening_8t. No algorithm changes.
Resume a stopped series with --resume; completed measurements are never replaced.
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

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
BASE = OUT.parent / "screening_8t"
IMPLEMENTATIONS = {"fixed", "balanced", "dynamic_dp"}


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def save(name, data):
    temporary = OUT / (name + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(OUT / name)


def read_rows(path):
    if not path.exists():
        return []
    with path.open() as stream:
        return list(csv.DictReader(stream))


def verify(record):
    rows = read_rows(OUT / record["csv"])
    expected = {flag[2:].replace("-", "_"): record["command"][i + 1]
                for i, flag in enumerate(record["command"][:-1]) if flag.startswith("--")}
    if len(rows) != 3 or {r["implementation"] for r in rows} != IMPLEMENTATIONS:
        raise ValueError("Expected one complete matched triple")
    for row in rows:
        for key in ("scenario", "threads", "operations", "trace_size", "array_size",
                    "hot_width", "range_length", "sample_rate"):
            if row[key] != expected[key]:
                raise ValueError(f"Mismatched {key}")
        if row["verified"] != "true" or row["repetition"] != "1" or float(row["wall_s"]) <= 0:
            raise ValueError("Failed or invalid measurement")
        if row["rebuild_interval_ms"] != expected["rebuild_ms"]:
            raise ValueError("Mismatched rebuild interval")
    if len({(r["seed"], r["unique_queries"]) for r in rows}) != 1:
        raise ValueError("Unmatched input")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    baseline = json.loads((BASE / "metadata.json").read_text())
    hashes = {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest()
              for p in baseline["source_sha256"]}
    if hashes != baseline["source_sha256"]:
        raise SystemExit("Measured sources/binary differ from the reference 8-worker series")
    for name, expected_hash in baseline["artifact_sha256"].items():
        if hashlib.sha256((BASE / name).read_bytes()).hexdigest() != expected_hash:
            raise SystemExit("Reference measurements have changed: " + name)
    if (OUT / "runs.json").exists():
        if not args.resume:
            raise SystemExit("Existing measurements: use --resume to skip completed sets")
        records = json.loads((OUT / "runs.json").read_text())
        metadata = json.loads((OUT / "metadata.json").read_text())
    else:
        if args.resume:
            raise SystemExit("No existing series to resume")
        records = []
        for index, old in enumerate(json.loads((BASE / "runs.json").read_text())):
            assert old["status"] == "complete"
            counts = [4, 6, 10]
            counts = counts[index % 3:] + counts[:index % 3]
            for workers in counts:
                name = old["scenario"]
                relative = f"{workers}t/{name}.csv"
                command = list(old["command"])
                command[command.index("--threads") + 1] = str(workers)
                command[command.index("--csv") + 1] = str(OUT / relative)
                records.append({"scenario": name, "threads": workers, "csv": relative,
                                "command": command, "status": "pending"})
        metadata = {
            "started_utc": stamp(), "platform": platform.platform(),
            "source_sha256": hashes, "reference": "../screening_8t",
            "reference_csv_sha256": baseline["artifact_sha256"],
            "workers_new": [4, 6, 10], "workers_reused": [8],
            "repetitions": 1, "sequential": True,
            "compiler": baseline["compiler"], "build": baseline["build"],
            "validation": "Same source and executable hashes as the 8-worker series, whose 6/6 CTest checks passed.",
            "method": [
                "Only --threads and output path differ from the 8-worker commands.",
                "Fixed operation counts, same workload distributions, lengths, seed and phase counts across worker counts.",
                "Per-worker input streams and work shares vary with worker count; within each triple all inputs are identical.",
                "Each implementation starts cold; wall/CPU include sampling, rebuilding, worker creation/join and lock destruction.",
                "Rebuild check interval 30 seconds, no forced rebuild at phase changes.",
                "Only one benchmark process at a time; adaptive variants also have one background rebuilder thread.",
                "Worker-count execution order rotates by scenario; implementation order remains unchanged.",
                "8-worker points were measured in the preceding series, not concurrently with this one.",
                "One repetition per point; curves do not imply statistical significance or monotonic scaling.",
            ],
        }
        save("metadata.json", metadata)
        save("runs.json", records)
    for index, record in enumerate(records, 1):
        if record["status"] == "complete":
            verify(record)
            continue
        path = OUT / record["csv"]
        path.parent.mkdir(exist_ok=True)
        # Preserve an interrupted attempt before repeating its entire matched set.
        for previous in (path, path.with_suffix(".txt")):
            if previous.exists():
                previous.rename(previous.with_name(previous.name + f".interrupted-{time.time_ns()}"))
        record.update(status="running", started_utc=stamp(), start_epoch=time.time())
        save("runs.json", records)
        print(f"[{index}/42] START {record['scenario']} {record['threads']}t {record['started_utc']}", flush=True)
        with path.with_suffix(".txt").open("w") as log:
            process = subprocess.Popen(record["command"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            record["pid"] = process.pid
            save("runs.json", records)
            reported = set()
            while True:
                returncode = process.poll()
                for row in read_rows(path):
                    implementation = row.get("implementation")
                    if implementation in IMPLEMENTATIONS and implementation not in reported and row.get("verified") == "true":
                        reported.add(implementation)
                        print(f"  {record['scenario']} {record['threads']}t {implementation}: "
                              f"{int(row['operations'])/float(row['wall_s'])/1e6:.3f} Mqps, "
                              f"{float(row['wall_s']):.2f}s, rebuilds={row['rebuilds']}, verified", flush=True)
                if returncode is not None:
                    break
                time.sleep(1)
        record.update(returncode=returncode, finished_utc=stamp(), elapsed_s=time.time()-record["start_epoch"])
        try:
            if returncode != 0:
                raise ValueError(f"Process exit {returncode}")
            verify(record)
            record["status"] = "complete"
        except (ValueError, KeyError) as error:
            record.update(status="failed", error=str(error))
        save("runs.json", records)
        print(f"[{index}/42] {record['status'].upper()} {record['scenario']} {record['threads']}t "
              f"{record['elapsed_s']:.1f}s", flush=True)
        if record["status"] != "complete":
            raise SystemExit("Stopped after failed matched set")
    metadata.update(finished_utc=stamp(), verified_new_runs=126,
                    elapsed_s=sum(r["elapsed_s"] for r in records))
    save("metadata.json", metadata)


if __name__ == "__main__":
    main()
