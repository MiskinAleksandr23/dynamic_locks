#!/usr/bin/env python3
"""Display every raw repetition, including failed/incomplete NUMA suites."""

import csv
import math
import sys
from pathlib import Path


REQUIRED = {"scenario", "repetition", "implementation", "wall_s", "cpu_s",
            "rebuilds", "rebuild_ms", "verified"}


def cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ") if value else "—"


def positive_number(value):
    try:
        number = float(value)
        return number if math.isfinite(number) and number > 0 else None
    except (TypeError, ValueError):
        return None


def verified(row):
    return (row.get("verified") or "").lower() in {"1", "true", "yes"}


def summarize(path):
    status_path = path.with_suffix(".status")
    status = status_path.read_text().strip() if status_path.exists() else "UNKNOWN"
    print(f"\n## {cell(path.stem)}\n\nStatus: **{cell(status)}**\n")
    if not path.exists():
        print("No raw CSV results. See the retained `.txt` log.\n")
        return False
    with path.open(newline="") as source:
        reader = csv.DictReader(source)
        if not REQUIRED.issubset(reader.fieldnames or []):
            print("CSV schema missing required fields; raw results retained.\n")
            return False
        rows = list(reader)
    if not rows:
        print("CSV has no completed measurements.\n")
        return False

    # Match only the same scenario/repetition/configuration. No averaging hides a loss.
    pairing = ("scenario", "repetition", "threads", "operations", "array_size",
               "hot_width", "range_length", "seed", "sample_rate", "rebuild_interval_ms",
               "trace_size", "unique_queries")
    fixed = {}
    groups = {}
    for row in rows:
        key = tuple(row.get(field) for field in pairing)
        groups.setdefault(key, set()).add(row.get("implementation"))
        if row.get("implementation") == "fixed":
            fixed.setdefault(key, []).append(row)

    print("| Scenario | Repeat | Lock | Wall s | CPU s | Speedup | Rebuilds | Rebuild ms | Verified |")
    print("|---|---:|---|---:|---:|---:|---:|---:|---|")
    for row in rows:
        speedup = "—"
        baseline = fixed.get(tuple(row.get(field) for field in pairing), [])
        if row.get("implementation") in {"balanced", "dynamic_dp", "dynamic"} and len(baseline) == 1:
            fixed_wall = positive_number(baseline[0].get("wall_s"))
            dynamic_wall = positive_number(row.get("wall_s"))
            if verified(row) and verified(baseline[0]) and fixed_wall and dynamic_wall:
                speedup = f"{fixed_wall / dynamic_wall:.3f}x"
        fields = [row.get("scenario"), row.get("repetition"), row.get("implementation"),
                  row.get("wall_s"), row.get("cpu_s"), speedup, row.get("rebuilds"),
                  row.get("rebuild_ms"), row.get("verified")]
        print("| " + " | ".join(cell(value) for value in fields) + " |")
    expected = {"fixed", "balanced", "dynamic_dp"}
    if any(row.get("implementation") == "dynamic" for row in rows):
        expected = {"fixed", "dynamic"}  # Retained CSV from the former two-way runner.
    for key, recorded in groups.items():
        for missing in sorted(expected - recorded):
            fields = [key[0], key[1], missing, "—", "—", "—", "—", "—", "MISSING"]
            print("| " + " | ".join(cell(value) for value in fields) + " |")
    return True


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    print(f"# NUMA benchmark results\n\nDirectory: `{root}`\n")
    print("Speedup = fixed wall time / variant wall time for the same verified repetition. "
          "balanced and dynamic_dp are each compared with that repetition’s fixed run. "
          "Wall time includes online adaptation. Failed suite results are incomplete; "
          "missing measurements are shown as —. All recorded repetitions appear below. "
          "The raw CSV files retain parameters, checksums and phase timings.")
    paths = {path.with_suffix(".csv") for pattern in ("*.csv", "*.status", "*.txt")
             for path in root.glob(pattern) if path.name != "topology.txt"}
    if not paths:
        print("\nNo benchmark results found.")
        return 1
    valid = True
    for path in sorted(paths):
        try:
            valid = summarize(path) and valid
        except (OSError, csv.Error) as error:
            print(f"\nCannot read `{path.name}`: {error}")
            valid = False
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
