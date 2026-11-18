#!/usr/bin/env python3
"""Finish the two Xeon comparison tables using their existing measurements."""
import fcntl
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    results = root / "BENCHMARK_RESULT"
    suites = (("--length-sweep", results / "moving-length-sweep-xeon5"),
              ("--mixed-only", results / "mixed-point-range-xeon5"))
    for _, folder in suites:
        if not (folder / "manifest.json").is_file():
            sys.exit(f"Missing previous run: {folder / 'manifest.json'}")

    with (results / "finish-comparisons.lock").open("a") as queue:
        try:
            fcntl.flock(queue, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            sys.exit("Final comparisons are already queued.")
        print("Waiting for active Table 1 runs to finish.", flush=True)
        for path in sorted(results.glob("*/.run.lock")):
            with path.open() as lock:
                fcntl.flock(lock, fcntl.LOCK_SH)

        failed = False
        for preset, folder in suites:
            run = subprocess.run([sys.executable, "-u", str(root / "scripts/full_table.py"),
                                  preset, "--output", str(folder), "--resume", "--execute"], cwd=root)
            if run.returncode:
                print(f"Check {preset} (exit {run.returncode}) in {folder}", flush=True)
                failed = True
        if failed:
            return 1
        report = results / "final-comparisons.md"
        tmp = report.with_suffix(".md.tmp")
        tmp.write_text("\n\n".join((folder / "TABLE.md").read_text().rstrip() for _, folder in suites) + "\n")
        tmp.replace(report)
        print(f"COMPLETE {report}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
