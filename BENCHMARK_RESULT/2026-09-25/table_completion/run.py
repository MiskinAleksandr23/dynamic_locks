#!/usr/bin/env python3
"""Complete only the missing Table 1 cells on 4 and 10 workers, one repetition."""
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
BINARY = ROOT / "build/genetic-release/adaptive_lock_benchmark"
TABLE = ROOT / "BENCHMARK_SHORT.md"
IMPL = ("balanced", "dynamic_dp", "genetic", "global", "per_element")
MATCH = ("scenario", "threads", "operations", "repetition", "trace_size", "unique_queries",
         "array_size", "hot_width", "range_length", "sample_rate", "rebuild_interval_ms", "seed")
ENGLISH = dict(zip(
    ("uniform", "clusters_2", "clusters_8", "unequal", "shift", "hot_with_spike",
     "steady_range", "clusters_2_range", "clusters_4_range", "islands_range", "gradient_range",
     "unequal_widths_range", "exchange_range", "moving_variable_range", "sliding_variable_range"),
    ("Uniform point", "Two clusters point", "Eight clusters point", "Unequal point", "Moving point",
     "Hot point + spike", "Steady range", "Two clusters range", "Four clusters range", "Range islands",
     "Range gradient", "Unequal-width range", "Popularity exchange", "Moving variable range",
     "Sliding variable range")))


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def save(name, value):
    temporary = OUT / (name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(OUT / name)


def read_rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def fixed_row(path):
    return next(r for r in read_rows(path) if r["implementation"] == "fixed" and r["repetition"] == "1")


def render_english():
    source = TABLE.read_text()
    lines = ["Table 1: Throughput relative to fixed (higher is better).", ""]
    for workers, label in ((4, "(a) Four worker threads"), (10, "(b) Ten worker threads")):
        lines += [f"**{label}**", "", "| Workload | Q (M) | Bal. | DP | Gen. | Global | Element |",
                  "|---|---:|---:|---:|---:|---:|---:|"]
        for row in source.splitlines():
            if not row.startswith("| `"):
                continue
            parts = [p.strip() for p in row.split("|")]
            scenario = parts[1].split('`')[1]
            label = ENGLISH[scenario] + (" (short)" if "(1 млн)" in parts[1] else "")
            if scenario == "moving_variable_range" and "(1 млн)" in parts[1]:
                label = "Moving variable (short)"
            values = parts[{4: 4, 10: 7}[workers]].replace(",", ".").split(" / ")
            assert len(values) == 5
            lines.append(f"| {label} | {parts[3]} | " + " | ".join(values) + " |")
        lines.append("")
    lines += ["Q is the total number of queries across all workers. Rebuild checks: 30 s for full workloads, "
              "10 s for sliding variable range, 100 ms for the three short workloads. "
              "Full-length Element range tests are intentionally omitted (—). Cold start, rebuilds, construction and destruction are included.", "",
              "Previously completed cells are retained. New short-workload ratios use newly measured fixed runs "
              "with matching inputs and worker counts. New hot-point and sliding ratios use the stored matching fixed runs. "
              "One repetition per new measurement; measurements from different dates are combined.", ""]
    (ROOT / "TABLE_1.md").write_text("\n".join(lines))


def update_cells(job, ratios):
    text = TABLE.read_text()
    lines = text.splitlines(keepends=True)
    prefix = f"| `{job['scenario']}`"
    candidates = [i for i, line in enumerate(lines) if line.startswith(prefix)
                  and ("(1 млн)" in line) == job["short"]]
    if len(candidates) != 1:
        raise ValueError("Expected one destination table row")
    index = candidates[0]
    parts = lines[index].split("|")
    column = {4: 4, 10: 7}[job["threads"]]
    values = parts[column].strip().split(" / ")
    if len(values) != 5:
        raise ValueError("Expected five implementation columns")
    for implementation, ratio in ratios.items():
        slot = IMPL.index(implementation)
        if values[slot] != "—":
            raise ValueError("Refusing to overwrite a completed table cell")
        values[slot] = f"{ratio:.2f}".replace(".", ",")
    parts[column] = " " + " / ".join(values) + " "
    lines[index] = "|".join(parts)
    temporary = OUT / "table-update.tmp"
    temporary.write_text("".join(lines))
    temporary.replace(TABLE)
    render_english()


def main():
    if (OUT / "manifest.json").exists():
        raise SystemExit("Refusing to overwrite an existing experiment")
    previous = json.loads((ROOT / "BENCHMARK_RESULT/2026-09-22/sliding_fixed_10s_10t/manifest.json").read_text())
    hashes = {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in previous["sha256"]}
    if hashes != previous["sha256"]:
        raise SystemExit("Sources/binary differ from the saved experiment")
    for name in ("common/element_lock.hpp", "genetic/adaptive_lock.hpp"):
        if (ROOT / name).exists():
            hashes[str(ROOT / name)] = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
    (OUT / "before.md").write_text(TABLE.read_text())
    jobs = []
    # The six short combinations need a matched fixed reference, absent previously.
    for workers in (4, 10):
        for scenario in ("steady_range", "clusters_2_range", "moving_variable_range"):
            jobs.append({"scenario": scenario, "threads": workers, "short": True,
                         "implementations": ["fixed", *IMPL], "reference": None,
                         "operations": 1000000, "rebuild_ms": 100})
    for workers in (4, 10):
        jobs.append({"scenario": "hot_with_spike", "threads": workers, "short": False,
                     "implementations": ["global", "per_element"], "operations": 2000000000,
                     "rebuild_ms": 30000,
                     "reference": f"BENCHMARK_RESULT/2026-09-18/scaling_4_6_8_10/{workers}t/hot_with_spike.csv"})
    jobs.append({"scenario": "sliding_variable_range", "threads": 10, "short": False,
                 "implementations": ["balanced", "dynamic_dp", "global"], "operations": 180000000,
                 "rebuild_ms": 10000,
                 "reference": "BENCHMARK_RESULT/2026-09-22/sliding_fixed_10s_10t/sliding_variable_range.csv"})
    for job in jobs:
        name = f"{job['scenario']}_{'short' if job['short'] else 'full'}_{job['threads']}t"
        job.update(status="pending", csv=name + ".csv", log=name + ".txt")
        ranges = job["scenario"].endswith("_range")
        # Point range_length was 8 in the original reference (effective query length is 1).
        length = fixed_row(ROOT / job["reference"])["range_length"] if job["reference"] else "10000"
        command = [str(BINARY), "--scenario", job["scenario"], "--threads", str(job["threads"]),
                   "--implementations", ",".join(job["implementations"]), "--repetitions", "1",
                   "--operations", str(job["operations"]), "--rebuild-ms", str(job["rebuild_ms"]),
                   "--array-size", str(1 << (26 if ranges else 20)),
                   "--hot-width", str(1 << (22 if ranges else 14)), "--range-length", length,
                   "--sample-rate", "64", "--trace-size", "1048576", "--seed", "1",
                   "--csv", str(OUT / job["csv"])]
        job["command"] = command
        if job["reference"]:
            job["reference_sha256"] = hashlib.sha256((ROOT / job["reference"]).read_bytes()).hexdigest()
    manifest = {"status": "running", "started_utc": stamp(), "source_sha256": hashes,
                "validation": "Saved Release binary/source hashes match 22 September; 9/9 CTest passed before this series.",
                "expected_new_measurements": 43, "repetitions": 1, "sequential": True, "jobs": jobs,
                "method": "Only missing cells measured; new fixed references only for the six short combinations. Every comparison has matching seed, geometry, trace budget and operation count. No long-range per_element runs. Full lock lifetime timed; final arrays verified. Previous measurements not overwritten."}
    save("manifest.json", manifest)
    render_english()
    for index, job in enumerate(jobs, 1):
        job.update(status="running", started_utc=stamp(), start_epoch=time.time())
        save("manifest.json", manifest)
        print(f"[{index}/{len(jobs)}] START {job['scenario']} {job['threads']}t {'short' if job['short'] else 'full'}", flush=True)
        with (OUT / job["log"]).open("w") as log:
            process = subprocess.Popen(job["command"], cwd=ROOT, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True)
            job["pid"] = process.pid
            save("manifest.json", manifest)
            for line in process.stdout:
                log.write(line)
                log.flush()
                if "verified=true" in line:
                    print(line.strip(), flush=True)
            code = process.wait()
        job.update(returncode=code, finished_utc=stamp(), elapsed_s=time.time()-job["start_epoch"])
        try:
            if code:
                raise ValueError(f"Process exit {code}")
            rows = read_rows(OUT / job["csv"])
            if len(rows) != len(job["implementations"]) or {r["implementation"] for r in rows} != set(job["implementations"]):
                raise ValueError("Missing or duplicate measurements")
            fixed = fixed_row(ROOT / job["reference"]) if job["reference"] else next(r for r in rows if r["implementation"] == "fixed")
            if job["reference"] and hashlib.sha256((ROOT / job["reference"]).read_bytes()).hexdigest() != job["reference_sha256"]:
                raise ValueError("Reference changed during experiment")
            for row in rows:
                if row["verified"] != "true" or row["repetition"] != "1" or float(row["wall_s"]) <= 0:
                    raise ValueError("Invalid measurement")
                if any(row[k] != fixed[k] for k in MATCH):
                    raise ValueError("Mismatched settings or input seed")
            ratios = {r["implementation"]: float(fixed["wall_s"])/float(r["wall_s"]) for r in rows if r["implementation"] != "fixed"}
            job.update(ratios=ratios, reference_wall_s=float(fixed["wall_s"]), status="verified",
                       csv_sha256=hashlib.sha256((OUT / job["csv"]).read_bytes()).hexdigest())
            save("manifest.json", manifest)
            update_cells(job, ratios)
            job["status"] = "complete"
        except (ValueError, KeyError) as error:
            job.update(status="failed", error=str(error))
            manifest["status"] = "failed"
            save("manifest.json", manifest)
            raise
        save("manifest.json", manifest)
        print("UPDATED " + json.dumps(job["ratios"]), flush=True)
    for name, digest in hashes.items():
        if hashlib.sha256(Path(name).read_bytes()).hexdigest() != digest:
            raise ValueError("Measured source/binary changed")
    manifest.update(status="complete", finished_utc=stamp(), verified_new_measurements=43,
                    elapsed_s=sum(j["elapsed_s"] for j in jobs))
    save("manifest.json", manifest)


if __name__ == "__main__":
    main()
