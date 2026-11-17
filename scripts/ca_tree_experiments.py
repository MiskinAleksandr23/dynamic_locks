#!/usr/bin/env python3
"""Separate, same-JVM author CA-tree versus fixed AVL-map experiment.

Each measurement runs in a fresh JVM. Never merge these numbers into the C++
array-lock table: both the language and the protected data structure differ.
"""
import argparse
import csv
import datetime as dt
import hashlib
import json
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "third_party/ca_tree_java"
HARNESS = ROOT / "benchmarks/java/CATreeRangeBenchmark.java"
CLASS = "se.uu.collection.CATreeRangeBenchmark"
MATCH = ("scenario", "threads", "keys", "hot_width", "range_length", "operations",
         "trace_size", "unique_queries", "seed", "trace_hash", "warmup_operations",
         "checksum", "java_version", "jvm_arguments")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stamp():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def executable(value):
    found = shutil.which(value)
    if not found:
        raise SystemExit(f"Executable not found: {value}; select a JDK 17+ with --java/--javac")
    return str(Path(found).resolve())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--java", default="java")
    p.add_argument("--javac", default="javac")
    p.add_argument("--threads", type=int, default=8)
    p.add_argument("--repetitions", type=int, default=3)
    p.add_argument("--seed", type=int, default=197)
    p.add_argument("--keys", type=int, default=1 << 20)
    p.add_argument("--hot-width", type=int, default=1 << 16)
    p.add_argument("--length", type=int, default=1000)
    p.add_argument("--trace-size", type=int, default=1 << 16)
    p.add_argument("--range-operations", type=int, default=8_000_000)
    p.add_argument("--point-operations", type=int, default=100_000_000)
    p.add_argument("--warmup-operations", type=int, default=200_000)
    p.add_argument("--timeout-seconds", type=int, default=600)
    p.add_argument("--cases", nargs="+", choices=["steady_range", "islands_range", "uniform_point"],
                   default=["steady_range", "islands_range", "uniform_point"])
    p.add_argument("--self-test-only", action="store_true")
    a = p.parse_args()
    if a.threads <= 0 or a.repetitions <= 0 or a.timeout_seconds <= 0:
        p.error("threads, repetitions and timeout must be positive")
    java, javac = executable(a.java), executable(a.javac)
    provenance = json.loads((VENDOR / "source.json").read_text())
    for entry in provenance["files"]:
        if digest(VENDOR / "upstream" / entry["path"]) != entry["sha256"]:
            raise SystemExit(f"Author source changed: {entry['path']}")
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = out / "sources"
    shutil.copytree(VENDOR, sources / "ca_tree_java")
    shutil.copy2(HARNESS, sources / HARNESS.name)
    shutil.copy2(Path(__file__), sources / Path(__file__).name)
    classes = out / "classes"
    classes.mkdir()
    flags = ["-Xms512m", "-Xmx2g", "-XX:+UseG1GC",
             f"-XX:ActiveProcessorCount={a.threads}", "-XX:-UseNUMA"]
    base = [java, *flags, "-cp", str(classes), CLASS]
    metadata = {"status": "building", "started_utc": stamp(), "platform": platform.platform(),
                "settings": {k: str(v) if isinstance(v, Path) else v for k, v in vars(a).items()},
                "java": java, "javac": javac, "jvm_flags": flags,
                "author_commit": provenance["commit"], "jobs": [],
                "method": "Fresh JVM per measurement, same author AVL storage and lock primitive. Separate-instance JVM warmup. Measured CA starts with one base node; all adaptation included. Every key checked by independent oracle. Dense population, trace generation and verification excluded. No comparison to C++ arrays. CA and fixed have different release schedules. Cyclic traces; unique_queries counts stored entries, not distinct intervals."}

    def save():
        temporary = out / "manifest.json.tmp"
        temporary.write_text(json.dumps(metadata, indent=2) + "\n")
        temporary.replace(out / "manifest.json")

    save()
    try:
        metadata["java_version"] = subprocess.check_output([java, "-version"], stderr=subprocess.STDOUT, text=True)
        metadata["javac_version"] = subprocess.check_output([javac, "-version"], stderr=subprocess.STDOUT, text=True)
        inputs = sorted((sources / "ca_tree_java/upstream/src/main/java").rglob("*.java"))
        inputs.append(sources / HARNESS.name)
        command = [javac, "--release", "17", "-d", str(classes), *map(str, inputs)]
        metadata["build_command"] = command
        with (out / "build.log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        metadata["class_sha256"] = {str(f.relative_to(classes)): digest(f) for f in sorted(classes.rglob("*.class"))}
        metadata["source_sha256"] = {str(f.relative_to(sources)): digest(f) for f in sorted(sources.rglob("*")) if f.is_file()}
        with (out / "self_test.log").open("w") as log:
            subprocess.run([*base, "--self-test"], stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        metadata["self_test"] = "passed"
        metadata["status"] = "running"
        save()
        rows = []
        for case in ([] if a.self_test_only else a.cases):
            for rep in range(1, a.repetitions + 1):
                # Alternate first implementation; fresh JVMs avoid sharing a heap/JIT profile.
                implementations = ["fixed_avl", "ca"] if rep % 2 else ["ca", "fixed_avl"]
                paired = []
                for impl in implementations:
                    if any(digest(classes / f) != h for f, h in metadata["class_sha256"].items()):
                        raise RuntimeError("Compiled class changed during series")
                    name = f"{case}_r{rep}_{impl}"
                    count = a.point_operations if case == "uniform_point" else a.range_operations
                    command = [*base, "--implementation", impl, "--scenario", case,
                               "--threads", str(a.threads), "--keys", str(a.keys),
                               "--hot-width", str(a.hot_width), "--length", str(a.length),
                               "--operations", str(count), "--trace-size", str(a.trace_size),
                               "--seed", str(a.seed + rep - 1), "--warmup-operations", str(a.warmup_operations),
                               "--timeout-seconds", str(a.timeout_seconds), "--csv", str(out / (name + ".csv"))]
                    job = {"name": name, "repetition": rep, "command": command,
                           "status": "running", "started_utc": stamp()}
                    metadata["jobs"].append(job)
                    save()
                    print("START", name, flush=True)
                    started = time.monotonic()
                    with (out / (name + ".log")).open("w") as log:
                        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                        job["pid"] = process.pid
                        save()
                        try:
                            code = process.wait(timeout=2 * a.timeout_seconds + 120)
                        except BaseException:
                            process.kill()
                            process.wait()
                            raise
                    job.update(returncode=code, elapsed_s=time.monotonic() - started)
                    if code:
                        raise RuntimeError(f"{name} failed: see its log")
                    with (out / (name + ".csv")).open() as stream:
                        values = list(csv.DictReader(stream))
                    if len(values) != 1 or values[0]["verified"] != "true":
                        raise RuntimeError(f"{name} incomplete or unverified")
                    row = dict(values[0], repetition=rep)
                    if impl == "ca" and row["route_nodes_before"] != "0":
                        raise RuntimeError("Measured CA was already adapted")
                    paired.append(row)
                    rows.append(row)
                    job.update(status="complete", finished_utc=stamp())
                    save()
                    print((out / (name + ".log")).read_text().strip(), flush=True)
                if len({tuple(row[key] for key in MATCH) for row in paired}) != 1:
                    raise RuntimeError(f"Unmatched input/settings: {case} repeat {rep}")
            values = [row for row in rows if row["scenario"] == case]
            ratios = []
            for rep in range(1, a.repetitions + 1):
                pair = {r["implementation"]: r for r in values if r["repetition"] == rep}
                ratios.append(float(pair["ca"]["qps"]) / float(pair["fixed_avl"]["qps"]))
            print(f"COMPLETE {case}: median paired CA/fixed_avl={statistics.median(ratios):.3f}x", flush=True)
        if rows:
            with (out / "results.csv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
        metadata.update(status="complete", finished_utc=stamp())
        save()
    except BaseException as error:
        metadata.update(status="failed", error=str(error), finished_utc=stamp())
        if metadata["jobs"] and metadata["jobs"][-1]["status"] == "running":
            metadata["jobs"][-1]["status"] = "failed"
        save()
        raise


if __name__ == "__main__":
    main()
