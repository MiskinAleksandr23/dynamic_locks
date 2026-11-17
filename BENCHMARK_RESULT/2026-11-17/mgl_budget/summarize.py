#!/usr/bin/env python3
"""Validate supplemental MGL budgets and compare seed-paired stored measurements.

Writes only TABLE.md and summary.json beside this script. It never launches a
benchmark or changes a source CSV/manifest. Run from any working directory.
"""
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics

OUT = Path(__file__).resolve().parent
REFERENCE = OUT.parent / "mgl_comparison"
ROOT = OUT.parents[2]
REPETITIONS = (1, 2, 3)
POLICIES = ("fixed", "balanced", "dynamic_dp", "mgl")
MATCH = (
    "scenario", "repetition", "threads", "operations", "trace_size", "unique_queries",
    "array_size", "hot_width", "range_length", "seed", "sample_rate", "rebuild_interval_ms",
    "lock_kind", "read_percent", "read_operations", "write_operations", "model", "bins",
    "lambda", "min_samples", "min_improvement", "write_body", "point_percent",
    "point_operations", "range_operations",
)
NUMERIC_MATCH = set(MATCH) - {"scenario", "model", "lock_kind", "write_body", "min_improvement"}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_rows(path):
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        required = set(MATCH) | {"implementation", "wall_s", "cpu_s", "verified", "phase_wall_s",
                                 "mgl_leaves", "hierarchy_leaves", "hierarchy_nodes", "hierarchy_bytes",
                                 "effective_lock_kind"}
        require(required <= set(reader.fieldnames or ()), f"{path}: missing fields")
        rows = list(reader)
    require(rows, f"{path}: no measurements")
    for row in rows:
        where = f"{path.name}:{row['repetition']}:{row['implementation']}"
        require(row["verified"] == "true", f"{where}: unverified array")
        for key in NUMERIC_MATCH | {"mgl_leaves", "hierarchy_leaves", "hierarchy_nodes", "hierarchy_bytes"}:
            row[key] = int(row[key])
            require(row[key] >= 0, f"{where}: negative {key}")
        row["min_improvement"] = float(row["min_improvement"])
        for key in ("wall_s", "cpu_s"):
            row[key] = float(row[key])
            require(math.isfinite(row[key]) and row[key] > 0, f"{where}: invalid {key}")
        require(row["repetition"] in REPETITIONS, f"{where}: unexpected repetition")
        require(row["operations"] > 0 and row["threads"] > 0, f"{where}: invalid work count")
        require(row["read_operations"] + row["write_operations"] == row["operations"],
                f"{where}: read/write count mismatch")
        require(row["point_operations"] + row["range_operations"] == row["operations"],
                f"{where}: point/range count mismatch")
        phases = [float(value) for value in row["phase_wall_s"].split(";")]
        require(len(phases) == 4 and all(math.isfinite(x) and x > 0 for x in phases),
                f"{where}: expected four positive phase times")
        require(sum(phases) <= row["wall_s"] * 1.00001 + 1e-6, f"{where}: phases exceed total time")
        row["qps"] = row["operations"] / row["wall_s"]
    return rows


def signature(row):
    return tuple(row[key] for key in MATCH)


def summarize_values(values):
    return {"median": statistics.median(values), "min": min(values), "max": max(values),
            "values_by_repetition": values}


def main():
    manifest_path = OUT / "manifest.json"
    reference_manifest_path = REFERENCE / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    reference_manifest = json.loads(reference_manifest_path.read_text())
    require(manifest["status"] == reference_manifest["status"] == "complete", "Both series must be complete")
    require(manifest["verified_runs"] == 12, "Expected 12 supplemental measurements")
    require(all(j["status"] == "complete" and j["returncode"] == 0 for j in manifest["jobs"]),
            "Supplemental job failed or incomplete")
    source = manifest["source_sha256"]
    reference_source = reference_manifest["source_sha256"]
    for key in ("common/multigranularity_lock.hpp", "benchmarks/adaptive_lock_benchmark.cpp"):
        require(source[key] == reference_source[key], f"Measured source differs between series: {key}")
    require(source["build/with-mgl/adaptive_lock_benchmark"] == reference_manifest["binary_sha256"],
            "Measured binary differs between series")

    scenarios = list(dict.fromkeys(j["scenario"] for j in manifest["jobs"]))
    require(set(scenarios) == {"steady_range", "islands_range"}, "Unexpected workload set")
    require({(j["scenario"], j["leaves"]) for j in manifest["jobs"]} ==
            {(s, leaves) for s in scenarios for leaves in (64, 32)}, "Missing or duplicate budget jobs")
    require(len(manifest["jobs"]) == 4, "Expected four budget jobs")
    results = []
    reference_results = []
    input_hashes = {str(manifest_path.relative_to(ROOT)): digest(manifest_path),
                    str(reference_manifest_path.relative_to(ROOT)): digest(reference_manifest_path)}
    settings = {}
    for scenario in scenarios:
        base_path = REFERENCE / (scenario + ".csv")
        base_job = next(j for j in reference_manifest["jobs"] if j["scenario"] == scenario)
        require(base_job["status"] == "complete", "Reference job incomplete")
        require(digest(base_path) == base_job["csv_sha256"], "Reference CSV hash mismatch")
        base = read_rows(base_path)
        require(len(base) == 12, "Reference must have three matched quartets")
        require(all(r["scenario"] == scenario for r in base), "Reference scenario mismatch")
        indexed = {(r["repetition"], r["implementation"]): r for r in base}
        require(set(indexed) == {(rep, implementation) for rep in REPETITIONS for implementation in POLICIES},
                "Missing or duplicate reference measurement")
        for rep in REPETITIONS:
            require(len({signature(indexed[rep, implementation]) for implementation in POLICIES}) == 1,
                    "Reference quartet metadata mismatch")
        first = indexed[1, "fixed"]
        require(first["threads"] == 8 and first["operations"] == 24_000_000 and first["array_size"] == 1 << 26
                and first["hot_width"] == 1 << 22 and first["range_length"] == 10000
                and first["write_body"] == "range" and first["read_percent"] == first["point_percent"] == 0
                and first["lambda"] == 10000 and first["rebuild_interval_ms"] == 10000,
                "Unexpected comparison settings")
        settings[scenario] = {k: first[k] for k in MATCH if k not in ("repetition", "seed")}
        input_hashes[str(base_path.relative_to(ROOT))] = digest(base_path)
        for implementation in ("fixed", "balanced", "dynamic_dp"):
            selected = [indexed[rep, implementation] for rep in REPETITIONS]
            reference_results.append({
                "scenario": scenario, "implementation": implementation,
                "throughput_relative_to_fixed": summarize_values([
                    indexed[rep, "fixed"]["wall_s"] / indexed[rep, implementation]["wall_s"]
                    for rep in REPETITIONS]),
                "qps": summarize_values([r["qps"] for r in selected]),
                "wall_s": summarize_values([r["wall_s"] for r in selected]),
            })
        for leaves in (1024, 64, 32):
            if leaves == 1024:
                path = base_path
                selected = [indexed[rep, "mgl"] for rep in REPETITIONS]
                cohort = "original matched comparison"
            else:
                job = next(j for j in manifest["jobs"] if j["scenario"] == scenario and j["leaves"] == leaves)
                require((ROOT / job["reference"]).resolve() == base_path.resolve(), "Unexpected fixed reference")
                command = job["command"]
                require(command[command.index("--mgl-leaves") + 1] == str(leaves), "Command budget mismatch")
                require(command[command.index("--implementations") + 1] == "mgl", "Unexpected implementation")
                path = OUT / f"{scenario}_{leaves}leaves.csv"
                selected = read_rows(path)
                require(len(selected) == 3 and {r["repetition"] for r in selected} == set(REPETITIONS)
                        and all(r["implementation"] == "mgl" for r in selected), "Invalid supplemental measurements")
                selected.sort(key=lambda r: r["repetition"])
                input_hashes[str(path.relative_to(ROOT))] = digest(path)
                cohort = "later sequential supplemental budget run"
            for row in selected:
                require(signature(row) == signature(indexed[row["repetition"], "fixed"]),
                        f"{path.name}: trace/geometry/settings differ from paired fixed")
                require(row["effective_lock_kind"] == "atomic_mgl" and row["mgl_leaves"] == leaves
                        and row["hierarchy_leaves"] == leaves and row["hierarchy_nodes"] == 2 * leaves - 1
                        and row["hierarchy_bytes"] == (2 * leaves - 1) * 128,
                        f"{path.name}: hierarchy metadata mismatch")
            wall = [row["wall_s"] for row in selected]
            results.append({
                "scenario": scenario, "leaves": leaves, "nodes": 2 * leaves - 1,
                "node_storage_bytes": (2 * leaves - 1) * 128,
                "leaf_width_elements": first["array_size"] // leaves,
                "source": str(path.relative_to(ROOT)), "cohort": cohort,
                "seeds_by_repetition": [row["seed"] for row in selected],
                "wall_s": summarize_values(wall), "qps": summarize_values([row["qps"] for row in selected]),
                "cpu_s": summarize_values([row["cpu_s"] for row in selected]),
                "throughput_relative_to_fixed": summarize_values([
                    indexed[rep, "fixed"]["wall_s"] / wall[rep - 1] for rep in REPETITIONS]),
                "throughput_relative_to_balanced": summarize_values([
                    indexed[rep, "balanced"]["wall_s"] / wall[rep - 1] for rep in REPETITIONS]),
                "throughput_relative_to_dp": summarize_values([
                    indexed[rep, "dynamic_dp"]["wall_s"] / wall[rep - 1] for rep in REPETITIONS]),
                "balanced_throughput_relative_to_mgl": summarize_values([
                    wall[rep - 1] / indexed[rep, "balanced"]["wall_s"] for rep in REPETITIONS]),
                "dp_throughput_relative_to_mgl": summarize_values([
                    wall[rep - 1] / indexed[rep, "dynamic_dp"]["wall_s"] for rep in REPETITIONS]),
            })
    summary = {
        "status": "complete", "input_sha256": input_hashes,
        "verified_supplemental_measurements": 12, "verified_reference_measurements_used": 24,
        "method": "Median of three seed/repetition-paired throughput ratios. The original 1024-leaf cohort measured all policies together in rotating order; 64/32-leaf supplemental runs occurred later and reuse those reference runs. No new fixed/Balanced/DP measurements and no equal-total-memory claim.",
        "settings": settings, "reference_results": reference_results, "mgl_results": results,
    }
    (OUT / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    refs = {(r["scenario"], r["implementation"]): r for r in reference_results}
    mgl = {(r["scenario"], r["leaves"]): r for r in results}
    lines = [
        "Throughput relative to fixed (higher is better).", "",
        "8 workers; 24M total requests per implementation per repetition; N=2²⁶, L=10 000; whole-range writes only. "
        "Paper policies: K=64, B=1024, λ=10000, sampling 1/64, rebuild check every 10 s. "
        "Values are medians of three ratios paired by seed/repetition; cold start and rebuilding are included.", "",
        "| Workload | Balanced / fixed | DP / fixed | MGL 1024 leaves / fixed | MGL 64 leaves / fixed | MGL 32 leaves / fixed |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for scenario in scenarios:
        values = [refs[scenario, implementation]["throughput_relative_to_fixed"]["median"]
                  for implementation in ("balanced", "dynamic_dp")]
        values += [mgl[scenario, leaves]["throughput_relative_to_fixed"]["median"] for leaves in (1024, 64, 32)]
        lines.append(f"| {scenario} | " + " | ".join(f"{v:.2f}" for v in values) + " |")
    lines += ["", "Direct comparisons: **our policy / MGL throughput** (>1 means our policy is faster).", "",
              "| Workload | MGL leaves | Node states | Balanced / MGL | DP / MGL |",
              "|---|---:|---:|---:|---:|"]
    for r in results:
        lines.append(f"| {r['scenario']} | {r['leaves']} | {r['nodes']} | "
                     f"{r['balanced_throughput_relative_to_mgl']['median']:.2f} | "
                     f"{r['dp_throughput_relative_to_mgl']['median']:.2f} |")
    lines += ["", "MGL hierarchy sizes: **1024 leaves / 2047 states / 262016 bytes**; "
              "**64 leaves / 127 states / 16256 bytes**; **32 leaves / 63 states / 8064 bytes**. "
              "These bytes describe allocated padded hierarchy nodes only. A node contains an atomic mode state, "
              "not a std::mutex; the adaptive policies also store statistics and mapping data. "
              "Similar numbers of states are not evidence of equal total memory or equal primitive cost.", "",
              "The 64- and 32-leaf measurements were collected in later sequential supplemental runs. "
              "Their inputs, seeds and repetition numbers match the saved fixed/Balanced/DP runs, but the "
              "reference policies were not rerun in those cohorts. Differences may include machine-state drift. "
              "All 12 new final-array checks and 24 reference checks used here passed; no incomplete data are included.", "",
              "[Detailed ratios and observed ranges](summary.json) · [Supplemental commands](manifest.json) · "
              "[Original comparison](../mgl_comparison/TABLE.md)", "",
              "Reproduce this summary: `python3 BENCHMARK_RESULT/2026-11-17/mgl_budget/summarize.py`.", ""]
    (OUT / "TABLE.md").write_text("\n".join(lines))
    for scenario in scenarios:
        print(scenario)
        for leaves in (1024, 64, 32):
            r = mgl[scenario, leaves]
            print(f"  MGL{leaves}: /fixed={r['throughput_relative_to_fixed']['median']:.3f}; "
                  f"Balanced/MGL={r['balanced_throughput_relative_to_mgl']['median']:.3f}; "
                  f"DP/MGL={r['dp_throughput_relative_to_mgl']['median']:.3f}")
    print("Validated 12 supplemental + 24 reference measurements; wrote TABLE.md and summary.json.")


if __name__ == "__main__":
    main()
