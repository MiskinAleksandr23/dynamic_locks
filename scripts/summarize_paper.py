#!/usr/bin/env python3
"""Validate and summarize matched paper-model experiments using only the standard library.

Usage: python3 scripts/summarize_paper.py BENCHMARK_RESULT/2026-11-17/paper_validation
Use --partial to report only jobs explicitly marked complete in a running manifest.
Writes summary.json and summary.csv without changing measured CSVs or the manifest.
"""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import statistics

IMPLEMENTATIONS = ("fixed", "balanced", "dynamic_dp")
CONFIG = (
    "scenario", "threads", "operations", "trace_size", "array_size", "hot_width",
    "range_length", "sample_rate", "rebuild_interval_ms", "lock_kind", "read_percent",
    "model", "bins", "lambda", "min_samples", "min_improvement", "write_body", "point_percent",
)
MATCH = CONFIG + ("seed", "unique_queries", "read_operations", "write_operations",
                  "point_operations", "range_operations")
TEXT_FIELDS = {"scenario", "lock_kind", "model", "write_body"}
COUNTS = {
    "repetition", "rebuilds", "optimizations", "last_window_samples", "actual_bins", "read_checksum",
} | (set(MATCH) - TEXT_FIELDS - {"min_improvement"})
TIMES = (
    "wall_s", "cpu_s", "rebuild_ms", "optimize_ms", "publication_wait_ms",
    "publication_hold_ms", "max_publication_pause_ms",
)
REQUIRED = set(MATCH) | COUNTS | set(TIMES) | {
    "implementation", "verified", "phase_wall_s", "partition_cuts",
}
ARGUMENTS = {
    "--scenario": "scenario", "--threads": "threads", "--operations": "operations",
    "--trace-size": "trace_size", "--array-size": "array_size", "--hot-width": "hot_width",
    "--range-length": "range_length", "--sample-rate": "sample_rate",
    "--rebuild-ms": "rebuild_interval_ms", "--lock-kind": "lock_kind",
    "--read-percent": "read_percent", "--model": "model", "--bins": "bins",
    "--lambda": "lambda", "--min-samples": "min_samples", "--min-improvement": "min_improvement",
    "--write-body": "write_body", "--point-percent": "point_percent",
}
METRICS = (
    "repetitions", "mean_qps", "median_qps", "min_qps", "max_qps",
    "median_paired_speedup", "min_paired_speedup", "max_paired_speedup", "mean_paired_speedup",
    "median_wall_s", "min_wall_s", "max_wall_s", "median_cpu_s",
    "median_paired_cpu_ratio", "min_paired_cpu_ratio", "max_paired_cpu_ratio",
    "min_rebuilds", "max_rebuilds", "min_optimizations", "max_optimizations",
    "median_optimize_ms", "max_optimize_ms", "median_rebuild_ms", "max_rebuild_ms",
    "median_publication_wait_ms", "max_publication_wait_ms",
    "median_publication_hold_ms", "max_publication_hold_ms", "max_publication_pause_ms",
    "max_rebuilder_elapsed_fraction", "median_rebuilder_elapsed_fraction",
    "min_last_window_samples", "max_last_window_samples", "actual_bins",
)


def finite(value, label, positive=False):
    result = float(value)
    if not math.isfinite(result) or result < 0 or (positive and result == 0):
        raise ValueError(f"{label}: expected finite {'positive' if positive else 'nonnegative'} value")
    return result


def integer(value, label):
    result = int(value)
    if str(result) != str(value) or result < 0:
        raise ValueError(f"{label}: expected nonnegative decimal integer")
    return result


def arguments(command):
    if not isinstance(command, list) or not command:
        raise ValueError("job command must be a nonempty argument list")
    result = {}
    index = 1
    while index < len(command):
        flag = command[index]
        if not isinstance(flag, str) or not flag.startswith("--") or flag in result:
            raise ValueError(f"invalid or duplicate command flag: {flag!r}")
        if flag == "--partitions":
            result[flag] = True
            index += 1
            continue
        if index + 1 == len(command):
            raise ValueError(f"missing command argument for {flag}")
        result[flag] = command[index + 1]
        index += 2
    return result


def convert_row(raw, where):
    if raw.get("verified") != "true" or raw.get("implementation") not in IMPLEMENTATIONS:
        raise ValueError(f"{where}: unverified or unknown implementation")
    row = dict(raw)
    for name in COUNTS:
        row[name] = integer(row[name], f"{where} {name}")
    for name in TIMES:
        row[name] = finite(row[name], f"{where} {name}", positive=name in ("wall_s", "cpu_s"))
    row["min_improvement"] = finite(row["min_improvement"], f"{where} min_improvement")
    if row["min_improvement"] >= 1:
        raise ValueError(f"{where}: minimum improvement must be below 1")
    if row["model"] != "paper" or row["write_body"] != "range":
        raise ValueError(f"{where}: this report requires paper model and whole-range writes")
    if row["lock_kind"] not in ("mutex", "spinlock", "rwlock"):
        raise ValueError(f"{where}: unknown lock kind")
    for name in ("repetition", "threads", "operations", "trace_size", "unique_queries", "array_size",
                 "hot_width", "range_length", "sample_rate", "rebuild_interval_ms", "bins", "min_samples"):
        if row[name] == 0:
            raise ValueError(f"{where}: {name} must be positive")
    if row["read_percent"] > 100 or row["point_percent"] > 100:
        raise ValueError(f"{where}: invalid requested operation percentage")
    if row["read_operations"] + row["write_operations"] != row["operations"]:
        raise ValueError(f"{where}: read/write counts do not sum to the operation count")
    if row["point_operations"] + row["range_operations"] != row["operations"]:
        raise ValueError(f"{where}: point/range counts do not sum to the operation count")
    if row["read_percent"] in (0, 100) and row["read_operations"] != row["operations"] * row["read_percent"] // 100:
        raise ValueError(f"{where}: pure read/write setting disagrees with actual counts")
    if row["point_percent"] == 100 and row["point_operations"] != row["operations"]:
        raise ValueError(f"{where}: pure point setting disagrees with actual counts")
    row["phases"] = [finite(v, f"{where} phase duration", positive=True)
                     for v in row["phase_wall_s"].split(";")]
    if sum(row["phases"]) > row["wall_s"] * 1.00001 + 1e-6:
        raise ValueError(f"{where}: phase durations exceed the full wall time")
    # These are overlapping diagnostics of one background thread. Never add
    # them to wall time or interpret their ratio as throughput lost to rebuilding.
    tolerance = max(1e-6, row["rebuild_ms"] * 1e-5)
    if row["optimize_ms"] > row["rebuild_ms"] + tolerance:
        raise ValueError(f"{where}: optimization time exceeds total rebuild time")
    if row["publication_wait_ms"] + row["publication_hold_ms"] > row["rebuild_ms"] + tolerance:
        raise ValueError(f"{where}: publication subtimings exceed total rebuild time")
    if row["max_publication_pause_ms"] > row["rebuild_ms"] + tolerance:
        raise ValueError(f"{where}: maximum publication pause exceeds total rebuild time")
    if row["implementation"] != "fixed":
        if not 1 <= row["actual_bins"] <= row["bins"]:
            raise ValueError(f"{where}: invalid effective adaptive bin count")
        cuts = [integer(v, f"{where} partition cut") for v in row["partition_cuts"].split(";")]
        if len(cuts) < 2 or cuts[0] != 0 or cuts[-1] != row["array_size"] or any(a >= b for a, b in zip(cuts, cuts[1:])):
            raise ValueError(f"{where}: final cuts do not partition the entire array")
    row["qps"] = row["operations"] / row["wall_s"]
    return row


def summarize_job(directory, job, manifest):
    args = arguments(job.get("command"))
    if set(args.get("--implementations", "fixed,balanced,dynamic_dp").split(",")) != set(IMPLEMENTATIONS):
        raise ValueError(f"{job['name']}: expected fixed, balanced and dynamic_dp")
    configured = job.get("csv", args.get("--csv"))
    if not configured:
        raise ValueError(f"{job['name']}: missing measured CSV path")
    source = Path(configured)
    # A copied result directory can be summarized without its original absolute path.
    source = directory / source if not source.is_absolute() else (
        directory / source.name if (directory / source.name).exists() else source)
    source = source.resolve()
    if source in ((directory / "summary.csv").resolve(), (directory / "summary.json").resolve()):
        raise ValueError("summary output would overwrite a measurement")
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if job.get("csv_sha256") and digest != job["csv_sha256"]:
        raise ValueError(f"{source}: stored CSV hash mismatch")
    reader = csv.DictReader(io.StringIO(data.decode("utf-8")))
    if not REQUIRED.issubset(reader.fieldnames or ()):
        raise ValueError(f"{source}: missing columns {sorted(REQUIRED - set(reader.fieldnames or ())) }")
    rows, matched = [], {}
    for line, raw in enumerate(reader, 2):
        if None in raw or any(value is None for value in raw.values()):
            raise ValueError(f"{source}:{line}: malformed CSV row")
        row = convert_row(raw, f"{source}:{line}")
        rows.append(row)
        pair = matched.setdefault(row["repetition"], {})
        if row["implementation"] in pair:
            raise ValueError(f"{source}:{line}: duplicate repetition/implementation")
        pair[row["implementation"]] = row
        for flag, field in ARGUMENTS.items():
            if flag not in args:
                continue
            expected = args[flag]
            if field in COUNTS:
                expected = integer(expected, flag)
            elif field == "min_improvement":
                expected = finite(expected, flag)
            if row[field] != expected:
                raise ValueError(f"{source}:{line}: {field} differs from its recorded command")
    if not rows:
        raise ValueError(f"{source}: no measurements")
    count = integer(args.get("--repetitions", manifest.get("repetitions", len(matched))), "repetitions")
    if count == 0 or set(matched) != set(range(1, count + 1)) or len(rows) != 3 * count:
        raise ValueError(f"{source}: incomplete repetitions")
    if len({tuple(r[field] for field in CONFIG) for r in rows}) != 1:
        raise ValueError(f"{source}: settings changed between repetitions")
    if len({len(r["phases"]) for r in rows}) != 1:
        raise ValueError(f"{source}: phase counts differ")
    for repetition, pair in matched.items():
        if set(pair) != set(IMPLEMENTATIONS):
            raise ValueError(f"{source}: repetition {repetition} is not a complete matched triple")
        if len({tuple(r[field] for field in MATCH) for r in pair.values()}) != 1:
            raise ValueError(f"{source}: repetition {repetition} has mismatched inputs or settings")
        if pair["balanced"]["actual_bins"] != pair["dynamic_dp"]["actual_bins"]:
            raise ValueError(f"{source}: effective adaptive bin counts differ")
    results = []
    for implementation in IMPLEMENTATIONS:
        selected = [matched[n][implementation] for n in sorted(matched)]
        qps = [r["qps"] for r in selected]
        speedup = [matched[n]["fixed"]["wall_s"] / matched[n][implementation]["wall_s"] for n in sorted(matched)]
        cpu_ratio = [matched[n][implementation]["cpu_s"] / matched[n]["fixed"]["cpu_s"] for n in sorted(matched)]
        wall = [r["wall_s"] for r in selected]
        fraction = [r["rebuild_ms"] / (1000 * r["wall_s"]) for r in selected]
        if len({r["actual_bins"] for r in selected}) != 1:
            raise ValueError(f"{source}: effective bin count changed across repetitions")
        result = {"case": job["name"], **{field: selected[0][field] for field in CONFIG},
                  "implementation": implementation, "repetitions": count,
                  "mean_qps": statistics.mean(qps), "median_qps": statistics.median(qps),
                  "min_qps": min(qps), "max_qps": max(qps),
                  "median_paired_speedup": statistics.median(speedup),
                  "min_paired_speedup": min(speedup), "max_paired_speedup": max(speedup),
                  "mean_paired_speedup": statistics.mean(speedup),
                  "median_wall_s": statistics.median(wall), "min_wall_s": min(wall), "max_wall_s": max(wall),
                  "median_cpu_s": statistics.median(r["cpu_s"] for r in selected),
                  "median_paired_cpu_ratio": statistics.median(cpu_ratio),
                  "min_paired_cpu_ratio": min(cpu_ratio), "max_paired_cpu_ratio": max(cpu_ratio),
                  "max_publication_pause_ms": max(r["max_publication_pause_ms"] for r in selected),
                  "max_rebuilder_elapsed_fraction": max(fraction),
                  "median_rebuilder_elapsed_fraction": statistics.median(fraction),
                  "actual_bins": selected[0]["actual_bins"], "source": str(source), "source_sha256": digest}
        for key in ("rebuilds", "optimizations", "last_window_samples"):
            result["min_" + key] = min(r[key] for r in selected)
            result["max_" + key] = max(r[key] for r in selected)
        for key in ("optimize_ms", "rebuild_ms", "publication_wait_ms", "publication_hold_ms"):
            result["median_" + key] = statistics.median(r[key] for r in selected)
            result["max_" + key] = max(r[key] for r in selected)
        results.append(result)
    return {"name": job["name"], "source": str(source), "source_sha256": digest,
            "settings": {field: rows[0][field] for field in CONFIG}, "phase_count": len(rows[0]["phases"]),
            "inputs_by_repetition": [{"repetition": n, **{field: pair["fixed"][field] for field in MATCH if field not in CONFIG}}
                                     for n, pair in sorted(matched.items())], "results": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--partial", action="store_true", help="use only completed jobs from a running manifest")
    args = parser.parse_args()
    directory = args.directory.resolve()
    try:
        manifest_data = (directory / "manifest.json").read_bytes()
        manifest = json.loads(manifest_data)
        jobs = manifest.get("jobs")
        if not isinstance(jobs, list) or not jobs:
            raise ValueError("manifest needs a nonempty jobs list")
        if len({job.get("name") for job in jobs}) != len(jobs) or any(not job.get("name") for job in jobs):
            raise ValueError("job names must be unique and nonempty")
        complete = [job for job in jobs if job.get("status") == "complete"]
        if not args.partial and (manifest.get("status") != "complete" or len(complete) != len(jobs)):
            raise ValueError("experiment is incomplete; wait for completion or use --partial")
        cases = [summarize_job(directory, job, manifest) for job in complete]
        result = {"complete": manifest.get("status") == "complete" and len(complete) == len(jobs),
                  "manifest": str(directory / "manifest.json"),
                  "manifest_sha256": hashlib.sha256(manifest_data).hexdigest(),
                  "manifest_status_at_read": manifest.get("status"), "completed_cases": len(cases),
                  "total_cases": len(jobs), "verified_measurements": sum(3 * case["results"][0]["repetitions"] for case in cases),
                  "skipped_jobs": [{"name": job["name"], "status": job.get("status")} for job in jobs if job.get("status") != "complete"],
                  "method": {
                      "speedup": "Within each repetition: fixed.wall_s / variant.wall_s. Report median, mean and observed min/max of paired ratios; min/max is not a confidence interval.",
                      "cpu_ratio": "Within each repetition: variant.cpu_s / fixed.cpu_s.",
                      "qps": "Total operations / full wall_s. Mean and median summarize per-repetition throughput.",
                      "optimize_ms": "Accumulated optimization time per run; max_optimize_ms is the maximum run total, not the duration of the slowest individual optimization.",
                      "max_publication_pause_ms": "Maximum measured admission-closed publication pause across all included repetitions.",
                      "rebuilder_elapsed_fraction": "rebuild_ms / (1000 * wall_s): background rebuilder elapsed fraction, NOT lost throughput. Publication/optimization times are subsets of rebuild_ms and are never added to it or to wall_s.",
                      "last_window_samples": "Sample count of the last processed statistics window, not total sampled requests.",
                      "source_validation": "Recorded CSV hashes checked when present; fresh SHA-256 stored for every source. Current workspace sources/binary are not required to match this historical experiment.",
                  }, "cases": cases}
        fields = ("case", *CONFIG, "implementation", *METRICS, "source", "source_sha256")
        csv_buffer = io.StringIO(newline="")
        writer = csv.DictWriter(csv_buffer, fieldnames=fields)
        writer.writeheader()
        for case in cases:
            writer.writerows(case["results"])
        for name, contents in (("summary.json", json.dumps(result, ensure_ascii=False, indent=2) + "\n"),
                               ("summary.csv", csv_buffer.getvalue())):
            temporary = directory / (name + ".tmp")
            temporary.write_text(contents, encoding="utf-8")
            temporary.replace(directory / name)
    except (OSError, ValueError, KeyError, TypeError, csv.Error) as error:
        parser.error(str(error))
    for case in cases:
        parts = []
        for row in case["results"][1:]:
            label = "DP" if row["implementation"] == "dynamic_dp" else "balanced"
            parts.append(f"{label} {row['median_paired_speedup']:.3f}x "
                         f"[{row['min_paired_speedup']:.3f}, {row['max_paired_speedup']:.3f}]")
        print(case["name"] + ": " + "; ".join(parts))
    print(f"{'Complete' if result['complete'] else 'Partial'}: {result['verified_measurements']} verified runs, "
          f"{len(cases)}/{len(jobs)} cases. Ratios are median paired speedups; brackets are observed min/max.")


if __name__ == "__main__":
    main()
