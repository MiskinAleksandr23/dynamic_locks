#!/usr/bin/env python3
"""One sequential matched triple per worker count; fill only the existing table cells."""
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
REFERENCE = OUT.parent / "sliding_10s_8t"


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def save(name, value):
    temporary = OUT / (name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(OUT / name)


def update_table(workers, rows):
    values = [float(rows["fixed"]["wall_s"]) / float(rows[i]["wall_s"])
              for i in ("balanced", "dynamic_dp")]
    cell = " / ".join(f"{v:.2f}".replace(".", ",") for v in values)
    path = ROOT / "BENCHMARK_SHORT.md"
    text = path.read_text()
    lines = text.splitlines(keepends=True)
    matching = [n for n, line in enumerate(lines) if line.startswith("| `sliding_variable_range` |")]
    if len(matching) != 1:
        raise ValueError("Expected exactly one existing sliding row")
    index = matching[0]
    parts = lines[index].split("|")
    column = {4: 4, 6: 5, 10: 7}[workers]
    if len(parts) != 9 or parts[column].strip() != "—":
        raise ValueError("Refusing to replace a nonempty or unexpected table cell")
    parts[column] = " " + cell + " "
    lines[index] = "|".join(parts)
    temporary = OUT / "short-update.tmp"
    temporary.write_text("".join(lines))
    temporary.replace(path)
    return cell


def main():
    if (OUT / "runs.json").exists():
        raise SystemExit("Existing run manifest: refusing to overwrite measurements")
    reference = json.loads((REFERENCE / "metadata.json").read_text())
    for name, digest in reference["source_sha256"].items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest:
            raise SystemExit("Source/binary changed since the 8-worker experiment: " + name)
    if hashlib.sha256((REFERENCE / "results.csv").read_bytes()).hexdigest() != reference["results_csv_sha256"]:
        raise SystemExit("Reference measurements changed")
    metadata = {
        "status": "running", "started_utc": stamp(), "reference": "../sliding_10s_8t",
        "source_sha256": reference["source_sha256"], "workers": [4, 6, 10],
        "repetitions": 1, "sequential": True, "geometry": reference["geometry"],
        "validation": "Same source and binary as the 8-worker experiment; its Release build and 6/6 CTest passed.",
        "method": "Only worker count and output path differ from the reference command. Fixed total work; matched inputs per triple; whole lock lifetime timed. Update only numeric cells in the existing sliding_variable_range row after each verified triple.",
    }
    records = []
    for workers in (4, 6, 10):
        command = list(reference["command"])
        command[command.index("--threads") + 1] = str(workers)
        output = f"{workers}t.csv"
        command[command.index("--csv") + 1] = str(OUT / output)
        records.append({"threads": workers, "command": command, "csv": output, "status": "pending"})
    save("metadata.json", metadata)
    save("runs.json", records)
    for record in records:
        workers = record["threads"]
        record.update(status="running", started_utc=stamp(), start_epoch=time.time())
        save("runs.json", records)
        print(f"START {workers}t", flush=True)
        with (OUT / f"{workers}t.txt").open("w") as log:
            process = subprocess.Popen(record["command"], cwd=ROOT, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            record["pid"] = process.pid
            save("runs.json", records)
            for line in process.stdout:
                log.write(line)
                log.flush()
                if "verified=true" in line:
                    print(f"{workers}t: {line.split()[1]} verified", flush=True)
            code = process.wait()
        record.update(returncode=code, elapsed_s=time.time()-record["start_epoch"], finished_utc=stamp())
        try:
            if code != 0:
                raise ValueError(f"Benchmark exit {code}")
            with (OUT / record["csv"]).open() as stream:
                rows = list(csv.DictReader(stream))
            if len(rows) != 3 or {r["implementation"] for r in rows} != {"fixed", "balanced", "dynamic_dp"}:
                raise ValueError("Incomplete matched triple")
            for row in rows:
                if (row["verified"] != "true" or row["threads"] != str(workers) or
                    row["repetition"] != "1" or row["operations"] != "180000000" or
                    row["rebuild_interval_ms"] != "10000" or len(row["phase_wall_s"].split(";")) != 9):
                    raise ValueError("Invalid results or settings")
            if len({(r["seed"], r["unique_queries"]) for r in rows}) != 1:
                raise ValueError("Inputs differ within the triple")
            record["table_cell"] = update_table(workers, {r["implementation"]: r for r in rows})
            record["csv_sha256"] = hashlib.sha256((OUT / record["csv"]).read_bytes()).hexdigest()
            record["status"] = "complete"
        except (ValueError, KeyError) as error:
            record.update(status="failed", error=str(error))
        save("runs.json", records)
        if record["status"] != "complete":
            raise SystemExit(record["error"])
        print(f"UPDATED {workers}t: {record['table_cell']}", flush=True)
    metadata.update(status="complete", finished_utc=stamp(), verified_runs=9,
                    elapsed_s=sum(r["elapsed_s"] for r in records))
    save("metadata.json", metadata)


if __name__ == "__main__":
    main()
