#!/usr/bin/env python3

import re
import sys
from pathlib import Path


ROW_RE = re.compile(
    r"^\s*(naive|dynamic|genetic)\s+"
    r"([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+"
    r"([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)\s+"
    r"([0-9.]+)\s+(\d+)\s*/\s*(\d+)\s+(\d+)\s*$"
)


def parse_file(path: Path):
    scenario = None
    rows = {}
    status_path = path.with_suffix(".status")
    status = status_path.read_text().strip() if status_path.exists() else "UNKNOWN"

    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("scenario: "):
            scenario = line.split(":", 1)[1].strip()
            continue
        match = ROW_RE.match(line)
        if not match:
            continue
        impl = match.group(1)
        rows[impl] = {
            "total_s": float(match.group(2)),
            "total_x": float(match.group(3)),
            "setup_s": float(match.group(4)),
            "measured_s": float(match.group(5)),
            "lock_only_s": float(match.group(6)),
            "lock_x": float(match.group(7)),
            "avg_lock_us": float(match.group(8)),
            "lock_sum_s": float(match.group(9)),
            "avg_mutex": float(match.group(10)),
            "hot_l": int(match.group(11)),
            "hot_r": int(match.group(12)),
            "rebuilds": int(match.group(13)),
        }

    return {
        "file": path.name,
        "scenario": scenario or path.stem,
        "status": status,
        "rows": rows,
    }


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    files = sorted(root.glob("*.txt"))
    files = [p for p in files if p.name != "topology.txt"]

    print(f"# NUMA Benchmark Summary\n")
    print(f"Directory: `{root}`\n")
    print(
        "| File | Status | Scenario | Naive total_s | Dynamic total_x | Genetic total_x | "
        "Dynamic rebuilds | Genetic trains | Dynamic lock_sum_s | Genetic lock_sum_s |"
    )
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|")

    for path in files:
        parsed = parse_file(path)
        rows = parsed["rows"]
        naive = rows.get("naive", {})
        dynamic = rows.get("dynamic", {})
        genetic = rows.get("genetic", {})
        print(
            f"| `{parsed['file']}` | {parsed['status']} | `{parsed['scenario']}` | "
            f"{naive.get('total_s', 0.0):.3f} | "
            f"{dynamic.get('total_x', 0.0):.3f} | "
            f"{genetic.get('total_x', 0.0):.3f} | "
            f"{dynamic.get('rebuilds', 0)} | "
            f"{genetic.get('rebuilds', 0)} | "
            f"{dynamic.get('lock_sum_s', 0.0):.3f} | "
            f"{genetic.get('lock_sum_s', 0.0):.3f} |"
        )

    print("\nUse `total_x` as the headline speedup. If `lock_sum_s` is orders of "
          "magnitude larger than wall time, the run is dominated by waiting.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
