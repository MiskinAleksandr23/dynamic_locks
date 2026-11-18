#!/usr/bin/env python3
"""Reproduce all 18 submitted Table 1 workload rows on four xeon5 placements."""
import argparse
from collections import defaultdict
import csv
from dataclasses import asdict
import datetime as dt
import fcntl
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import time

from run_single_node import discover, eligible_cpus, verify_sysfs, command_output, digest

ROOT = Path(__file__).resolve().parents[1]
POLICIES = ("fixed", "balanced", "dynamic_dp", "global", "per_element")
# key, label, generator, reference Q, range length, phase count, short experiment
CASES = (
    ("uniform", "Uniform point", "uniform", 5_000_000_000, 1, 1, False),
    ("clusters_2", "Two clusters point", "clusters_2", 2_000_000_000, 1, 1, False),
    ("clusters_8", "Eight clusters point", "clusters_8", 2_000_000_000, 1, 1, False),
    ("unequal", "Unequal point", "unequal", 2_000_000_000, 1, 1, False),
    ("shift", "Moving point", "shift", 2_000_000_000, 1, 2, False),
    ("hot_with_spike", "Hot point + spike", "hot_with_spike", 2_000_000_000, 1, 4, False),
    ("steady_range", "Steady range", "steady_range", 64_000_000, 10_000, 4, False),
    ("clusters_2_range", "Two clusters range", "clusters_2_range", 64_000_000, 10_000, 1, False),
    ("clusters_4_range", "Four clusters range", "clusters_4_range", 64_000_000, 10_000, 1, False),
    ("islands_range", "Range islands", "islands_range", 96_000_000, 10_000, 4, False),
    ("gradient_range", "Range gradient", "gradient_range", 96_000_000, 10_000, 4, False),
    ("unequal_widths_range", "Unequal-width range", "unequal_widths_range", 96_000_000, 10_000, 4, False),
    ("exchange_range", "Popularity exchange", "exchange_range", 96_000_000, 10_000, 2, False),
    ("moving_variable_range", "Moving variable range", "moving_variable_range", 160_000_000, 10_000, 4, False),
    ("sliding_variable_range", "Sliding variable range", "sliding_variable_range", 180_000_000, 10_000, 9, False),
    ("steady_short", "Steady range (short)", "steady_range", 1_000_000, 10_000, 4, True),
    ("clusters_2_short", "Two clusters range (short)", "clusters_2_range", 1_000_000, 10_000, 1, True),
    ("moving_short", "Moving variable (short)", "moving_variable_range", 1_000_000, 10_000, 4, True),
)
PROFILES = ("16t", "32t-smt", "32t-physical", "64t-physical")
OLD = ROOT / "BENCHMARK_RESULT/server-paper-16-32-20261118-022122"


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def placements(rows, affinity, memory_nodes):
    physical = eligible_cpus(rows, affinity, memory_nodes, False)
    smt = eligible_cpus(rows, affinity, memory_nodes, True)
    for node in range(4):
        if len(physical.get(node, [])) < 16:
            raise ValueError(f"NUMA node {node} needs 16 permitted physical cores")
    if len(smt[0]) < 32:
        raise ValueError("NUMA node 0 needs 32 permitted logical CPUs for the SMT run")
    specs = (
        ("16t", physical[0][:16], [0]),
        ("32t-smt", smt[0][:32], [0]),
        ("32t-physical", physical[0][:16] + physical[1][:16], [0, 1]),
        ("64t-physical", sum((physical[n][:16] for n in range(4)), []), list(range(4))),
    )
    return {name: {"threads": len(selected), "cpus": [r.cpu for r in selected],
                  "topology": [asdict(r) for r in selected], "nodes": nodes,
                  "physical_cores": len({(r.socket, r.core) for r in selected}),
                  "memory_policy": "bind" if len(nodes) == 1 else "interleave"}
            for name, selected, nodes in specs}


def binding(profile, numactl):
    cpus = ",".join(map(str, profile["cpus"]))
    nodes = ",".join(map(str, profile["nodes"]))
    memory = "--membind" if len(profile["nodes"]) == 1 else "--interleave"
    return [numactl, "--physcpubind=" + cpus, memory + "=" + nodes]


def old_rows(case, profile, previous):
    key, _, scenario, reference_q, length, _, short = case
    if short or profile["name"] not in ("16t", "32t-smt"):
        return {}
    threads = profile["threads"]
    candidates = [
        (previous, previous / f"{threads}t"),
        (previous.with_name(previous.name + "-once") / f"placement-{threads}",
         previous.with_name(previous.name + "-once") / f"placement-{threads}/{threads}t"),
    ]
    for root, folder in candidates:
        path = folder / f"{key}.csv"
        placement = root / "placement.json"
        if not path.exists() or not placement.exists():
            continue
        placement_data = json.loads(placement.read_text())
        group = next((g for g in placement_data["jobs"] if g["threads"] == threads), {})
        if set(group.get("cpus", [])) != set(profile["cpus"]) or placement_data.get("node") != 0:
            raise ValueError(f"CPU placement differs in {path}")
        with path.open() as f:
            rows = list(csv.DictReader(f))
        full = defaultdict(dict)
        for row in rows:
            if row.get("verified") != "true":
                continue
            if (row["scenario"] != scenario or int(row["threads"]) != threads
                    or int(row["operations"]) != reference_q or row["model"] != "paper"
                    or row["write_body"] != "range" or row["lock_kind"] != "mutex"
                    or int(row["range_length"]) != length or int(row["read_percent"]) != 0
                    or int(row["point_percent"]) != 0 or row["lambda"] != "10000"
                    or int(row["array_size"]) != (2**26 if length > 1 else 2**20)
                    or int(row["hot_width"]) != (2**22 if length > 1 else 2**14)
                    or int(row["bins"]) != 1024 or int(row["sample_rate"]) != 64):
                raise ValueError(f"Unexpected prior settings in {path}")
            full[row["repetition"]][row["implementation"]] = row
        paired = [rep for rep in full.values() if all(p in rep for p in POLICIES[:3])]
        if paired:
            return {p: {"status": "complete", "method": "reused_counted",
                        "source": str(path), "source_sha256": digest(path),
                        "results": [r[p] for r in paired]} for p in POLICIES[:3]}
    return {}


def rate(job):
    return statistics.median(int(r["operations"]) / float(r["wall_s"]) for r in job["results"])


def report(out, manifest):
    jobs = {(j["profile"], j["case"], j["implementation"]): j for j in manifest["jobs"]}
    baseline = manifest.get("baseline")
    columns = (baseline,) if baseline else POLICIES[1:]
    cases = [case for case in CASES if not manifest.get("short_only") or case[-1]]
    scope = (f"Three short workload profiles, each measured for {manifest['jobs'][0]['seconds']:g} s per implementation. "
             if manifest.get("short_only") else "All 18 workload rows from the submitted PDF. ")
    lines = ["# Table 1 on xeon5", "",
             "Throughput relative to fixed. " + scope +
             "Q ref. is the original request count. New cells use fixed-duration runs and actual completed query counts. "
             "Reused cells retain the complete measured runs. Raw CSV and summary.csv record the method and counts.",
             "", "One NUMA node for 16t and 32t-smt. Two nodes for 32t-physical, four for 64t-physical. "
             "Every worker is pinned to one logical CPU. Memory is bound on one-node runs and interleaved on multi-node runs.", ""]
    for profile in PROFILES:
        labels = [baseline.upper()] if baseline else ["Bal.", "DP", "Global", "Element"]
        lines += [f"## {profile}", "", "| Workload | Q ref. (M) | " + " | ".join(labels) + " |",
                  "|---|---:|" + "---:|" * len(columns)]
        for key, label, _, q, *_ in cases:
            base = jobs[profile, key, "fixed"]
            values = []
            for p in columns:
                j = jobs[profile, key, p]
                if j["status"] != "complete" or base["status"] != "complete":
                    values.append(j["status"] if j["status"] != "complete" else f"fixed {base['status']}")
                    continue
                if j["method"] == base["method"] == "reused_counted":
                    fixed = {r["repetition"]: r for r in base["results"]}
                    ratio = statistics.median(float(fixed[r["repetition"]]["wall_s"]) / float(r["wall_s"])
                                              for r in j["results"])
                else:
                    ratio = rate(j) / rate(base)
                value = f"{ratio:.3f}" if ratio < .1 else f"{ratio:.2f}"
                if j["method"] == "timed" or base["method"] == "timed":
                    value += "†"
                values.append(value)
            lines.append("| " + " | ".join([label, f"{q / 1e6:g}", *values]) + " |")
        lines.append("")
    lines += ["† At least one value in the ratio comes from the new fixed-duration run. "
              "Every cell uses measured requests / full wall time. These ratios compare complete profiles "
              "with different executed counts and phase residence times. New ordinary runs use 5 s rebuild "
              "checks, short runs 100 ms. Reused counted runs retain their recorded intervals.", ""]
    (out / "TABLE.md").write_text("\n".join(lines) + "\n")
    fields = ["profile", "case", "implementation", "status", "method", "operations", "wall_s",
              "qps", "rebuilds", "first_publication_s", "last_publication_s", "source", "error", "optimizations"]
    with (out / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for j in manifest["jobs"]:
            for r in j.get("results") or [{}]:
                writer.writerow({**{k: j.get(k, "") for k in fields},
                                 **{k: r[k] for k in fields if k in r},
                                 "qps": int(r["operations"]) / float(r["wall_s"]) if r else ""})
    save(out / "manifest.json", manifest)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, help="new output directory, or existing directory for --resume")
    p.add_argument("--previous", type=Path, default=OLD)
    p.add_argument("--binary", type=Path, default=ROOT / "build/full-table/table1_benchmark")
    p.add_argument("--short-only", action="store_true", help="run only the three short profiles, each for --seconds")
    p.add_argument("--mgl64", dest="baseline", action="store_const", const="mgl64",
                   help="time MGL with 64 leaves and a fresh fixed reference for each workload")
    p.add_argument("--seconds", type=float, help="duration per implementation in ordinary rows or in --short-only mode")
    p.add_argument("--execute", action="store_true", help="run the displayed plan")
    p.add_argument("--resume", action="store_true", help="continue pending/interrupted cells in the existing output")
    args = p.parse_args()
    if args.seconds is None:
        args.seconds = 3 if args.baseline and not args.short_only else 20
    if not 1 <= args.seconds <= 120:
        p.error("seconds must be between 1 and 120")
    if platform.system() != "Linux":
        p.error("run this launcher on Linux")
    numactl, lscpu = shutil.which("numactl"), shutil.which("lscpu")
    if not numactl or not lscpu:
        p.error("numactl and lscpu are required")
    default_name = "short-table-xeon5" if args.short_only else "full-table-xeon5"
    if args.baseline:
        default_name += "-" + args.baseline
    binary, out = args.binary.resolve(), (args.output or ROOT / "BENCHMARK_RESULT" / default_name).resolve()
    rows, affinity, memory_nodes, topology = discover(lscpu)
    profiles = placements(rows, affinity, memory_nodes)
    for name, profile in profiles.items():
        profile["name"] = name
        for node in profile["nodes"]:
            verify_sysfs([r for r in rows if r.cpu in profile["cpus"] and r.node == node],
                         allow_smt=name == "32t-smt")
        print(f"{name}: {profile['threads']} workers, {profile['physical_cores']} physical cores, "
              f"CPUs={profile['cpus']}, memory {profile['memory_policy']} {profile['nodes']}", flush=True)
    if args.resume:
        manifest = json.loads((out / "manifest.json").read_text())
        if args.baseline and args.baseline != manifest.get("baseline"):
            p.error("selected baseline differs from the saved plan")
        if args.short_only and not manifest.get("short_only"):
            p.error("this is a full-series manifest; use a separate directory for --short-only")
        if manifest["profiles"] != profiles or manifest["binary_sha256"] != digest(binary):
            raise ValueError("Resume requires the same CPU placement and binary")
        for job in manifest["jobs"]:
            if job["status"] in ("running", "interrupted", "failed"):
                job["status"] = "pending"
    else:
        if out.exists():
            p.error("output exists; choose --resume or a new directory")
        manifest = {"status": "planned", "profiles": profiles, "topology": topology, "short_only": args.short_only,
                    "baseline": args.baseline,
                    "binary_sha256": digest(binary), "jobs": [],
                    "source_sha256": {str(f): digest(ROOT / f) for f in (
                        "scripts/full_table.py", "benchmarks/table1_benchmark.cpp",
                        "benchmarks/adaptive_lock_benchmark.cpp", "dynamic/paper_lock.hpp",
                        "dynamic/paper_partitioner.hpp", "common/multigranularity_lock.hpp")}}
        for profile_index, (name, profile) in enumerate(profiles.items()):
            for case_index, case in enumerate(CASES):
                if args.short_only and not case[-1]:
                    continue
                reused = {} if args.baseline else old_rows(case, profile, args.previous.resolve())
                seconds = 2 if case[-1] and not args.short_only else args.seconds
                if case[5] == 9:
                    seconds = max(9 if args.baseline else 60, seconds)
                policies = POLICIES
                if args.baseline:
                    policies = ("fixed", args.baseline) if (case_index + profile_index) % 2 == 0 else (args.baseline, "fixed")
                for impl in policies:
                    manifest["jobs"].append({"profile": name, "case": case[0], "implementation": impl,
                        **reused.get(impl, {"status": "pending", "method": "timed", "results": []}),
                        "seconds": seconds})
    pending = [j for j in manifest["jobs"] if j["status"] == "pending"]
    print(f"{len(manifest['jobs']) - len(pending)} existing cells, {len(pending)} new cells; "
          f"{sum(j['seconds'] for j in pending) / 60:.1f} minutes of timed work plus allocation/verification.",
          flush=True)
    if not args.execute:
        return
    out.mkdir(parents=True, exist_ok=args.resume)
    run_lock = (out / ".run.lock").open("a")
    try:
        fcntl.flock(run_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise RuntimeError("This output directory already has an active suite") from None
    manifest["lscpu"] = command_output([lscpu])
    manifest["numactl_hardware"] = command_output([numactl, "--hardware"])
    manifest["status"] = "running"
    report(out, manifest)
    cases = {c[0]: c for c in CASES}
    for job in pending:
        name, impl = job["profile"], job["implementation"]
        profile = profiles[name]
        key, _, scenario, _, length, phases, short = cases[job["case"]]
        directory = out / name / key
        directory.mkdir(parents=True, exist_ok=True)
        attempt = job.get("attempt", 0) + 1
        job["attempt"] = attempt
        suffix = "" if attempt == 1 else f".attempt{attempt}"
        csv_path = directory / f"{impl}{suffix}.csv"
        ranges = length > 1
        command = [*binding(profile, numactl), str(binary), "--seconds", str(job["seconds"]),
                   "--cpu-list", ",".join(map(str, profile["cpus"])), "--threads", str(profile["threads"]),
                   "--scenario", scenario, "--implementations", impl, "--repetitions", "1",
                   "--csv", str(csv_path), "--array-size", str(2**26 if ranges else 2**20),
                   "--hot-width", str(2**22 if ranges else 2**14), "--range-length", str(length),
                   "--model", "paper", "--write-body", "range", "--seed", "97",
                   "--trace-size", "1048576", "--rebuild-ms", "100" if short else "5000",
                   "--bins", "1024", "--lambda", "10000", "--min-samples", "640",
                   "--min-improvement", "0.1", "--sample-rate", "64"]
        job.update(status="running", command=command, source=str(csv_path))
        report(out, manifest)
        print(f"START {name} {key} {impl}", flush=True)
        started = time.monotonic()
        try:
            if digest(binary) != manifest["binary_sha256"]:
                raise RuntimeError("binary changed during series")
            show = command_output([*binding(profile, numactl), numactl, "--show"])
            if f"policy: {profile['memory_policy']}" not in show:
                raise RuntimeError("memory policy verification failed")
            job["numactl_show"] = show
            with (directory / f"{impl}{suffix}.log").open("w") as log:
                proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                        cwd=ROOT, start_new_session=True)
                try:
                    job["pid"] = proc.pid
                    save(out / "manifest.json", manifest)
                    code = proc.wait(timeout=job["seconds"] + 90)
                finally:
                    if proc.poll() is None:
                        os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
            if code:
                raise RuntimeError(f"benchmark exited {code}")
            with csv_path.open() as stream:
                result, = csv.DictReader(stream)
            if (result["verified"] != "true" or result["implementation"] != impl
                    or result["scenario"] != scenario or int(result["threads"]) != profile["threads"]
                    or int(result["range_length"]) != length):
                raise ValueError("unverified result or wrong implementation")
            if impl == "mgl64" and (result["hierarchy_leaves"], result["hierarchy_nodes"]) != ("64", "127"):
                raise ValueError("unexpected MGL64 geometry")
            counts = [int(n) for n in result["phase_operations"].split(";")]
            if len(counts) != phases or min(counts) <= 0 or sum(counts) != int(result["operations"]):
                raise ValueError("phase operation counts mismatch")
            job.update(status="complete", results=[result])
        except KeyboardInterrupt:
            job["status"] = manifest["status"] = "interrupted"
            report(out, manifest)
            return 130
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            job.update(status="failed", error=str(error))
        job["elapsed_s"] = time.monotonic() - started
        report(out, manifest)
        print(f"{job['status'].upper()} {name} {key} {impl} ({job['elapsed_s']:.1f}s)", flush=True)
    manifest["status"] = "complete" if all(j["status"] == "complete" for j in manifest["jobs"]) else "incomplete"
    report(out, manifest)
    print(f"{manifest['status'].upper()}: {out / 'TABLE.md'}", flush=True)
    return 0 if manifest["status"] == "complete" else 1


if __name__ == "__main__":
    def terminate(_signal, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, terminate)
    sys.exit(main())
