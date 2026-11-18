#!/usr/bin/env python3
"""Switch a Linux paper suite to one repeat for each unstarted (threads, case)."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

from paper_experiments import CASES
from run_single_node import digest

ROOT = Path(__file__).resolve().parents[1]
SUITE = ROOT / "scripts/paper_experiments.py"
PLACER = ROOT / "scripts/run_single_node.py"
FINISHED = {"complete", "failed", "interrupted"}


def read(path):
    return json.loads(path.read_text())


def remaining_cases(previous, threads):
    directory = previous / f"{threads}t"
    if not directory.exists():
        return list(CASES)
    manifest = read(directory / "manifest.json")
    if manifest["threads"] != threads:
        raise ValueError(f"Unexpected worker count in {directory}")
    started = set()
    for job in manifest["jobs"]:
        if job["status"] == "pending":
            continue
        if job["status"] not in FINISHED | {"running", "partial"}:
            raise ValueError(f"Unknown job status in {directory}: {job['status']}")
        started.add(job["name"])
    return [case for case in CASES if case not in started]


def stop_previous(previous):
    """Stop the exact native child. Both existing supervisors record its failure and exit."""
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        placement = read(previous / "placement.json")
        if placement["status"] in FINISHED:
            return
        if placement["status"] != "running":
            raise ValueError(f"Unknown placement status: {placement['status']}")
        for group in placement["jobs"]:
            if group["status"] != "running" or not group.get("pid"):
                continue
            directory = previous / f"{group['threads']}t"
            if not (directory / "manifest.json").exists():
                continue
            for job in read(directory / "manifest.json")["jobs"]:
                if job["status"] != "running" or not job.get("pid"):
                    continue
                pid, expected = job["pid"], job["command"]
                if Path(expected[0]).name != "adaptive_lock_benchmark" or Path(
                        expected[expected.index("--csv") + 1]).resolve() != directory / (job["name"] + ".csv"):
                    raise ValueError("Unexpected benchmark or output in prior manifest")
                try:
                    fd = os.pidfd_open(pid)
                    try:
                        proc = Path(f"/proc/{pid}")
                        args = [os.fsdecode(s) for s in (proc / "cmdline").read_bytes().split(b"\0") if s]
                        if (args == expected and os.getsid(pid) == group["pid"]
                                and (proc / "exe").resolve() == Path(expected[0]).resolve()):
                            signal.pidfd_send_signal(fd, signal.SIGTERM)
                            print(f"Stopping {group['threads']}t/{job['name']} (PID {pid})", flush=True)
                    finally:
                        os.close(fd)
                except (FileNotFoundError, ProcessLookupError):
                    pass  # Child finished between the manifest read and the signal.
        time.sleep(0.25)
    raise RuntimeError("Previous placement has not stopped. Check its manifest and processes before retrying.")


def suite_args(group, previous):
    # Reuse workload flags from the original suite invocation, including the seed.
    _, _, _, python, script, *args = group["command"]
    if Path(script).resolve() != SUITE or not Path(python).is_file():
        raise ValueError("Previous placement must run scripts/paper_experiments.py")
    def index(flag):
        return args.index(flag) + 1
    if Path(args[index("--output")]).resolve() != previous / f"{group['threads']}t":
        raise ValueError("Unexpected previous suite output")
    # This entry point continues the standard eight-case paper suite.
    if "--cases" in args:
        raise ValueError("Expected an original all-case suite")
    args[index("--output")] = "{output}"
    args[index("--threads")] = "{threads}"
    if "--repetitions" in args:
        args[index("--repetitions")] = "1"
    else:
        args.extend(["--repetitions", "1"])
    return [python, str(SUITE), *args]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("previous", type=Path, help="existing run_single_node.py output")
    parser.add_argument("--execute", action="store_true", help="stop the previous run and start the remainder")
    args = parser.parse_args()
    old = args.previous.resolve()
    source = read(old / "placement.json")
    if Path(source["cwd"]).resolve() != ROOT:
        raise ValueError("Run from the same checkout as the original series")
    os.chdir(ROOT)
    output = old.with_name(old.name + "-once")
    groups = source["jobs"]
    for group in groups:
        suite_args(group, old)
        print(f"{group['threads']}t pending:", *remaining_cases(old, group["threads"]), flush=True)
    if not args.execute:
        return
    if output.exists():
        raise ValueError(f"Remainder output already exists: {output}")
    for group in groups:
        for name, expected in group["command_files_sha256"].items():
            if digest(Path(name)) != expected:
                raise ValueError(f"Measured executable or suite changed: {name}")
    stop_previous(old)
    output.mkdir()
    remaining = {g["threads"]: remaining_cases(old, g["threads"]) for g in groups}
    (output / "plan.json").write_text(json.dumps({
        "previous": str(old), "repetitions": 1, "remaining": remaining,
    }, indent=2) + "\n")
    for group in groups:
        threads = group["threads"]
        cases = remaining[threads]
        if not cases:
            continue
        print(f"Starting {threads}t, one repetition:", *cases, flush=True)
        placement_dir = output / f"placement-{threads}"
        command = [sys.executable, str(PLACER), "--execute", "--node", str(source["node"]),
                   "--threads", str(threads), "--output", str(placement_dir)]
        if source.get("allow_smt"):
            command.append("--allow-smt")
        subprocess.run([*command, "--", *suite_args(group, old), "--cases", *cases], check=True)
    print("All remaining scenarios completed.", flush=True)


if __name__ == "__main__":
    main()
