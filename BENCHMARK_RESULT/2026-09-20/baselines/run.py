#!/usr/bin/env python3
"""Measure only new baselines against stored workloads; publish ratios after each run."""
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
OLD = ROOT / "BENCHMARK_RESULT/2026-09-18"
TABLE = ROOT / "BASELINE_COMPARISON.md"


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def save(name, value):
    temp = OUT / (name + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temp.replace(OUT / name)


def read_csv(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def render(records, descriptions):
    lines = [
        "Global — один mutex на весь запрос. Element — один mutex на элемент; измерен только для точек.", "",
        "Параметры и число запросов сохранены из [BENCHMARK_SHORT.md](BENCHMARK_SHORT.md). Один повтор. В ячейках **global / element**: "
        "запросы/с относительно прежнего **fixed** (>1 быстрее, <1 медленнее). «—» — не запускался. "
        + ("«…» — ожидается. " if any(r["status"] != "complete" for r in records) else "") +
        "Время включает создание/уничтожение структуры и весь запрос. Fixed взят из предыдущей серии.", "",
        "| Тест | Нагрузка | Q, млн | 4 потока | 6 потоков | 8 потоков | 10 потоков |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    for name, description, operations in descriptions:
        cells = []
        for workers in (4, 6, 8, 10):
            values = []
            for implementation in ("global", "per_element"):
                match = [r for r in records if r["scenario"] == name and r["threads"] == workers
                         and r["implementation"] == implementation]
                if not match:
                    values.append("—")
                elif match[0]["status"] == "complete":
                    values.append(f"{match[0]['speedup_vs_previous_fixed']:.2f}".replace(".", ","))
                else:
                    values.append("…")
            cells.append(" / ".join(values))
        lines.append(f"| `{name}` | {description} | {operations} | " + " | ".join(cells) + " |")
    temporary = OUT / "table.tmp"
    temporary.write_text("\n".join(lines) + "\n")
    temporary.replace(TABLE)


def main():
    if (OUT / "runs.json").exists():
        raise SystemExit("Existing measurements; refusing to overwrite")
    original = (ROOT / "BENCHMARK_SHORT.md").read_text()
    (OUT / "reference_table.md").write_text(original)
    descriptions = []
    for line in original.splitlines():
        if line.startswith("| `"):
            cells = [c.strip() for c in line.split("|")]
            scenario = cells[1].split("`")[1]
            if not any(item[0] == scenario for item in descriptions):
                descriptions.append((scenario, cells[2], cells[3]))
    records = []
    for scenario, _, _ in descriptions:
        for workers in ((4, 8) if scenario == "sliding_variable_range" else (4, 6, 8, 10)):
            if scenario == "sliding_variable_range":
                prior = OLD / ("sliding_10s_8t/results.csv" if workers == 8 else "sliding_10s_4_6_10/4t.csv")
            else:
                prior = OLD / (f"screening_8t/{scenario}.csv" if workers == 8 else
                               f"scaling_4_6_8_10/{workers}t/{scenario}.csv")
            row = next(r for r in read_csv(prior) if r["implementation"] == "fixed")
            for implementation in (("global",) if scenario.endswith("_range") else ("global", "per_element")):
                output = f"{scenario}_{workers}t_{implementation}.csv"
                command = [str(ROOT / "build/adaptive_lock_benchmark"), "--scenario", scenario,
                           "--threads", str(workers), "--repetitions", "1", "--seed", "1",
                           "--implementations", implementation, "--csv", str(OUT / output)]
                for flag, key in (("--operations", "operations"), ("--array-size", "array_size"),
                                  ("--hot-width", "hot_width"), ("--range-length", "range_length"),
                                  ("--sample-rate", "sample_rate"), ("--rebuild-ms", "rebuild_interval_ms"),
                                  ("--trace-size", "trace_size")):
                    command.extend((flag, row[key]))
                records.append({"scenario": scenario, "threads": workers, "implementation": implementation,
                                "csv": output, "command": command, "reference": str(prior.relative_to(ROOT)),
                                "reference_sha256": hashlib.sha256(prior.read_bytes()).hexdigest(),
                                "status": "pending"})
    sources = ["common/element_lock.hpp", "common/naive_lock.hpp", "common/range_lock.hpp",
               "dynamic/dynamic_lock.hpp", "dynamic/partitioner.hpp", "benchmarks/adaptive_lock_benchmark.cpp",
               "build/adaptive_lock_benchmark"]
    metadata = {"status": "running", "started_utc": stamp(), "expected_runs": len(records),
                "source_sha256": {p: hashlib.sha256((ROOT / p).read_bytes()).hexdigest() for p in sources},
                "validation": "Release build and 7/7 CTest passed including per-element concurrency, exception unwinding and all-scenario cyclic smoke for both baselines.",
                "method": "Only global/per_element measured. One repetition. Original counts, geometry, seed, worker count and trace budgets retained. Full final array verified. Global=NaiveLock<1>; per_element=dense std::mutex array, inclusive range held throughout callback. Per-element range workloads explicitly excluded at user request. Reference fixed measured earlier, not rerun."}
    save("metadata.json", metadata)
    save("runs.json", records)
    render(records, descriptions)
    for index, record in enumerate(records, 1):
        print(f"[{index}/{len(records)}] START {record['scenario']} {record['threads']}t {record['implementation']}", flush=True)
        record.update(status="running", started_utc=stamp(), start_epoch=time.time())
        save("runs.json", records)
        path = OUT / record["csv"]
        with path.with_suffix(".txt").open("w") as log:
            process = subprocess.Popen(record["command"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
            record["pid"] = process.pid
            save("runs.json", records)
            code = process.wait()
        record.update(returncode=code, elapsed_s=time.time()-record["start_epoch"], finished_utc=stamp())
        try:
            if code:
                raise ValueError(f"Exit {code}")
            rows = read_csv(path)
            if len(rows) != 1 or rows[0]["verified"] != "true" or rows[0]["implementation"] != record["implementation"]:
                raise ValueError("Invalid or incomplete measurement")
            row = rows[0]
            prior = ROOT / record["reference"]
            if hashlib.sha256(prior.read_bytes()).hexdigest() != record["reference_sha256"]:
                raise ValueError("Reference CSV changed")
            fixed = next(r for r in read_csv(prior) if r["implementation"] == "fixed")
            for key in ("scenario", "repetition", "threads", "operations", "trace_size", "unique_queries",
                        "array_size", "hot_width", "range_length", "seed", "sample_rate", "rebuild_interval_ms"):
                if row[key] != fixed[key]:
                    raise ValueError("Reference metadata mismatch: " + key)
            record.update(status="complete", wall_s=float(row["wall_s"]),
                          qps=int(row["operations"])/float(row["wall_s"]),
                          speedup_vs_previous_fixed=float(fixed["wall_s"])/float(row["wall_s"]),
                          csv_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        except (ValueError, KeyError) as error:
            record.update(status="failed", error=str(error))
        save("runs.json", records)
        render(records, descriptions)
        if record["status"] != "complete":
            raise SystemExit(record["error"])
        print(f"DONE {record['scenario']} {record['threads']}t {record['implementation']}: "
              f"{record['qps']/1e6:.3f} Mqps, {record['wall_s']:.2f}s, "
              f"ratio={record['speedup_vs_previous_fixed']:.3f}", flush=True)
    metadata.update(status="complete", finished_utc=stamp(), verified_runs=len(records),
                    elapsed_s=sum(r["elapsed_s"] for r in records))
    save("metadata.json", metadata)


if __name__ == "__main__":
    main()
