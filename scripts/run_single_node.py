#!/usr/bin/env python3
r"""Plan or run sequential benchmarks on physical cores of one Linux NUMA node.

Dry-run is the default. Example (run from the repository root):
  python3 scripts/run_single_node.py --threads 32 64 --node auto \
    --output BENCHMARK_RESULT/server-check -- \
    python3 scripts/paper_experiments.py --threads '{threads}' --output '{output}'

Add --execute before -- to execute the printed plan. Every process, including
coordinator/rebuilder threads, inherits the selected CPU set and memory policy.
This is process affinity, not a unique worker-to-core assignment. No server is
contacted, no SMT sibling substitutes for a physical core, and no node fallback
or memory-policy fallback is performed.
"""
from __future__ import annotations

import argparse
import csv
from dataclasses import asdict, dataclass
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time


@dataclass(frozen=True)
class CPU:
    cpu: int
    core: int
    socket: int
    node: int
    online: bool


def parse_id_list(text: str) -> set[int]:
    """Parse the Linux '0-3,8,10-12' representation, without shell expansion."""
    result: set[int] = set()
    if not text.strip():
        return result
    for part in text.strip().split(","):
        if not re.fullmatch(r"[0-9]+(?:-[0-9]+)?", part):
            raise ValueError(f"invalid Linux ID list: {text!r}")
        limits = [int(value) for value in part.split("-")]
        first, last = limits[0], limits[-1]
        if last < first:
            raise ValueError(f"reversed ID range: {part}")
        result.update(range(first, last + 1))
    return result


def parse_lscpu(text: str) -> list[CPU]:
    rows = []
    seen = set()
    for values in csv.reader(line for line in text.splitlines()
                             if line.strip() and not line.lstrip().startswith("#")):
        if len(values) != 5:
            raise ValueError("lscpu must provide CPU,CORE,SOCKET,NODE,ONLINE")
        online_text = values[4].strip().lower()
        if online_text not in ("y", "yes", "1", "true", "n", "no", "0", "false"):
            raise ValueError(f"unknown lscpu ONLINE value: {values[4]!r}")
        online = online_text in ("y", "yes", "1", "true")
        numbers = []
        for value in values[:4]:
            value = value.strip()
            numbers.append(-1 if value in ("", "-") else int(value))
        cpu, core, socket, node = numbers
        if cpu < 0 or cpu in seen:
            raise ValueError("lscpu contains an invalid or duplicate CPU ID")
        seen.add(cpu)
        if online and min(core, socket, node) < 0:
            raise ValueError("online CPU lacks a physical-core/socket/NUMA-node identity")
        rows.append(CPU(cpu, core, socket, node, online))
    if not rows:
        raise ValueError("empty lscpu topology")
    # Do not count a physical core twice if topology labels are inconsistent.
    nodes_by_core: dict[tuple[int, int], set[int]] = {}
    for row in rows:
        if row.online:
            nodes_by_core.setdefault((row.socket, row.core), set()).add(row.node)
    if any(len(nodes) != 1 for nodes in nodes_by_core.values()):
        raise ValueError("a physical core spans multiple NUMA node IDs; refusing ambiguous topology")
    return rows


def status_field(text: str, name: str) -> str:
    matches = [line.partition(":")[2].strip() for line in text.splitlines()
               if line.startswith(name + ":")]
    if len(matches) != 1:
        raise ValueError(f"/proc/self/status lacks unique {name}")
    return matches[0]


def eligible_cores(rows: list[CPU], affinity: set[int], memory_nodes: set[int]) -> dict[int, list[CPU]]:
    """One permitted online logical CPU per physical (socket, core)."""
    representatives: dict[int, dict[tuple[int, int], CPU]] = {}
    for row in rows:
        if not row.online or row.cpu not in affinity or row.node not in memory_nodes:
            continue
        by_core = representatives.setdefault(row.node, {})
        key = row.socket, row.core
        if key not in by_core or row.cpu < by_core[key].cpu:
            by_core[key] = row
    return {node: sorted(by_core.values(), key=lambda c: (c.socket, c.core, c.cpu))
            for node, by_core in representatives.items()}


def select_node(rows: list[CPU], affinity: set[int], memory_nodes: set[int],
                requested: list[int], node: str = "auto") -> tuple[int, dict[int, list[CPU]]]:
    if not requested or any(n <= 0 for n in requested) or len(set(requested)) != len(requested):
        raise ValueError("worker counts must be positive and distinct")
    eligible = eligible_cores(rows, affinity, memory_nodes)
    largest = max(requested)
    if node == "auto":
        candidates = sorted(n for n, cores in eligible.items() if len(cores) >= largest)
        if not candidates:
            capacities = {n: len(cores) for n, cores in sorted(eligible.items())}
            raise ValueError(f"need {largest} permitted physical cores on ONE memory-allowed NUMA node; "
                             f"available by node: {capacities}. SMT siblings and other nodes will not be used")
        selected = candidates[0]
    else:
        if not re.fullmatch(r"[0-9]+", node):
            raise ValueError("--node must be auto or a nonnegative NUMA node ID")
        selected = int(node)
        if selected not in memory_nodes:
            raise ValueError(f"NUMA node {selected} is not in Mems_allowed_list")
        count = len(eligible.get(selected, []))
        if count < largest:
            raise ValueError(f"NUMA node {selected} has {count} permitted physical cores; "
                             f"{largest} requested. No cross-node/SMT fallback")
    pool = eligible[selected]
    return selected, {n: pool[:n] for n in requested}


def parse_numactl_show(text: str, cpus: list[int], node: int) -> None:
    values = {}
    for line in text.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            values[key.strip()] = value.strip()
    if values.get("policy") != "bind":
        raise ValueError("numactl did not confirm memory policy 'bind'")
    for name, expected in (("physcpubind", set(cpus)), ("membind", {node})):
        try:
            actual = {int(x) for x in values[name].split()}
        except (KeyError, ValueError) as error:
            raise ValueError(f"numactl --show did not report valid {name}") from error
        if actual != expected:
            raise ValueError(f"effective {name} {sorted(actual)} differs from requested {sorted(expected)}")


def verify_sysfs(selected: list[CPU], system: Path = Path("/sys/devices/system")) -> dict:
    """Cross-check the chosen CPUs against kernel online, sibling and node masks."""
    online_text = (system / "cpu/online").read_text().strip()
    online = parse_id_list(online_text)
    nodes = {row.node for row in selected}
    if len(nodes) != 1:
        raise ValueError("selected CPUs do not belong to exactly one NUMA node")
    node = next(iter(nodes))
    node_text = (system / f"node/node{node}/cpulist").read_text().strip()
    node_cpus = parse_id_list(node_text)
    siblings = {}
    occupied: set[int] = set()
    for row in selected:
        if row.cpu not in online or row.cpu not in node_cpus:
            raise ValueError(f"kernel sysfs disagrees with online/node placement of CPU {row.cpu}")
        text = (system / f"cpu/cpu{row.cpu}/topology/thread_siblings_list").read_text().strip()
        group = parse_id_list(text)
        if row.cpu not in group or occupied & group:
            raise ValueError("selected CPUs share a physical core or have invalid SMT sibling masks")
        occupied.update(group)
        siblings[str(row.cpu)] = text
    return {"online": online_text, "node": node, "node_cpulist": node_text,
            "thread_siblings_list": siblings}


def stamp() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def command_output(command: list[str]) -> str:
    env = dict(os.environ, LC_ALL="C")
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=env, check=False)
    if result.returncode:
        raise RuntimeError(f"{shlex.join(command)} failed ({result.returncode}): {result.stderr.strip()}")
    return result.stdout


def discover(lscpu: str) -> tuple[list[CPU], set[int], set[int], dict]:
    raw = command_output([lscpu, "--all", "--parse=CPU,CORE,SOCKET,NODE,ONLINE"])
    rows = parse_lscpu(raw)
    affinity = set(os.sched_getaffinity(0))
    status = Path("/proc/self/status").read_text()
    status_cpus = parse_id_list(status_field(status, "Cpus_allowed_list"))
    if affinity != status_cpus:
        raise RuntimeError("CPU affinity changed during preflight; rerun after placement is stable")
    memory_text = status_field(status, "Mems_allowed_list")
    memory_nodes = parse_id_list(memory_text)
    if not affinity or not memory_nodes:
        raise RuntimeError("empty permitted CPU or memory-node set")
    return rows, affinity, memory_nodes, {
        "lscpu_parse_command": [lscpu, "--all", "--parse=CPU,CORE,SOCKET,NODE,ONLINE"],
        "lscpu_parse_output": raw,
        "cpus_allowed": sorted(affinity),
        "mems_allowed_list": memory_text,
        "memory_nodes_allowed": sorted(memory_nodes),
        "cpus": [asdict(row) for row in rows],
    }


def executable_path(name: str, cwd: Path) -> Path:
    found = str((cwd / name).resolve()) if "/" in name else shutil.which(name)
    if not found or not Path(found).is_file() or not os.access(found, os.X_OK):
        raise ValueError(f"command executable is unavailable: {name}")
    return Path(found).resolve()


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while data := stream.read(1024 * 1024):
            result.update(data)
    return result.hexdigest()


def command_hashes(command: list[str], cwd: Path) -> dict[str, str]:
    paths = {executable_path(command[0], cwd)}
    # Capture script/Java artifact or an explicitly supplied benchmark binary,
    # without reading arbitrary input files or dumping the environment.
    if len(command) > 1 and Path(command[1]).suffix in (".py", ".sh"):
        paths.add((cwd / command[1]).resolve())
    for flag in ("--binary", "-jar"):
        if flag in command:
            index = command.index(flag)
            if index + 1 == len(command):
                raise ValueError(f"missing path after {flag}")
            paths.add((cwd / command[index + 1]).resolve())
    return {str(path): digest(path) for path in sorted(paths)}


def build_jobs(command: list[str], output: Path, cwd: Path, numactl: str,
               node: int, selections: dict[int, list[CPU]]) -> list[dict]:
    if not command or not any("{threads}" in arg for arg in command):
        raise ValueError("command after -- must contain {threads}")
    if not any("{output}" in arg for arg in command):
        raise ValueError("command after -- must contain {output} to keep run outputs separate")
    jobs = []
    for threads, selected in selections.items():
        cpus = [row.cpu for row in selected]
        cpu_list = ",".join(str(cpu) for cpu in cpus)
        replacement = {"{threads}": str(threads), "{output}": str(output / f"{threads}t"),
                       "{node}": str(node), "{cpus}": cpu_list}
        rendered = []
        for arg in command:
            for key, value in replacement.items():
                arg = arg.replace(key, value)
            rendered.append(arg)
        rendered[0] = str(executable_path(rendered[0], cwd))
        binding = [numactl, f"--physcpubind={cpu_list}", f"--membind={node}"]
        jobs.append({"threads": threads, "node": node, "cpus": cpus,
                     "physical_cores": [asdict(row) for row in selected],
                     "output_placeholder": str(output / f"{threads}t"),
                     "command": binding + rendered, "binding_command": binding,
                     "command_files_sha256": command_hashes(rendered, cwd), "status": "pending"})
    return jobs


def save_manifest(directory: Path, manifest: dict) -> None:
    temporary = directory / "placement.json.tmp"
    temporary.write_text(json.dumps(manifest, indent=2) + "\n")
    temporary.replace(directory / "placement.json")


def run_job(job: dict, output: Path, cwd: Path, numactl: str, save_state) -> int:
    # Verify effective policies in a bound child before starting the workload.
    verified = command_output(job["binding_command"] + [numactl, "--show"])
    parse_numactl_show(verified, job["cpus"], job["node"])
    job["verified_numactl_show"] = verified
    save_state()
    log_path = output / f"{job['threads']}t.console.log"
    job["console_log"] = log_path.name
    with log_path.open("x") as log:
        proc = subprocess.Popen(job["command"], cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
        try:
            job["pid"] = proc.pid
            save_state()
            return proc.wait()
        except KeyboardInterrupt:
            # The process has its own group; stop its benchmark children too.
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=10)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
            job["status"] = "interrupted"
            return 130


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--threads", type=int, nargs="+", default=[32, 64])
    parser.add_argument("--node", default="auto", help="one NUMA node ID, or auto (default)")
    parser.add_argument("--output", type=Path, required=True, help="new parent output directory")
    parser.add_argument("--cwd", type=Path, default=Path.cwd(), help="working directory for the command")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--execute", action="store_true", help="execute sequentially after preflight")
    modes.add_argument("--dry-run", action="store_true", help="print only; this is the default")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if platform.system() != "Linux" or not hasattr(os, "sched_getaffinity"):
        parser.error("placement discovery/execution requires Linux; run this preflight on the target server")
    output, cwd = args.output.resolve(), args.cwd.resolve()
    if output.exists():
        parser.error(f"refusing to overwrite existing output directory: {output}")
    if not cwd.is_dir():
        parser.error(f"working directory does not exist: {cwd}")
    lscpu, numactl = shutil.which("lscpu"), shutil.which("numactl")
    if not lscpu or not numactl:
        parser.error("lscpu and numactl are required; install util-linux and numactl on the server")
    try:
        rows, affinity, memory_nodes, topology = discover(lscpu)
        node, selections = select_node(rows, affinity, memory_nodes, args.threads, args.node)
        topology["selected_sysfs"] = verify_sysfs(selections[max(args.threads)])
        jobs = build_jobs(command, output, cwd, numactl, node, selections)
    except (ValueError, RuntimeError, OSError) as error:
        parser.error(str(error))
    print(f"NUMA node {node}; one online allowed logical CPU per physical core; no SMT siblings.", flush=True)
    print("All process threads share each CPU set (including coordinator/rebuilder); no one-to-one worker pinning.", flush=True)
    for job in jobs:
        print(f"{job['threads']} workers / {len(job['cpus'])} physical cores: CPUs="
              + ",".join(map(str, job["cpus"])) + f"; memory node={node}", flush=True)
        print(shlex.join(job["command"]), flush=True)
    if not args.execute:
        print("DRY RUN: no workload executed and no output directory created. Add --execute to run this plan.")
        return 0

    try:
        lscpu_summary = command_output([lscpu])
        numactl_hardware = command_output([numactl, "--hardware"])
        output.mkdir(parents=True, exist_ok=False)
    except (RuntimeError, OSError) as error:
        parser.error(str(error))
    relevant_env = ("OMP_NUM_THREADS", "OMP_PLACES", "OMP_PROC_BIND", "GOMP_CPU_AFFINITY",
                    "KMP_AFFINITY", "KMP_HW_SUBSET", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                    "NUMEXPR_NUM_THREADS", "LANG", "LC_ALL")
    manifest = {"status": "running", "started_utc": stamp(), "cwd": str(cwd),
                "platform": platform.platform(), "python": sys.version,
                "node": node, "requested_workers": args.threads, "topology": topology,
                "lscpu_summary": lscpu_summary,
                "numactl_hardware": numactl_hardware,
                "environment": {key: os.environ[key] for key in relevant_env if key in os.environ},
                "method": "Sequential. CPU affinity contains exactly one logical CPU per selected physical core, "
                          "all on one memory-allowed NUMA node. Memory policy is bind to that node, verified before "
                          "each workload. CPU subsets are nested for increasing worker counts. The coordinator and "
                          "background threads share this budget; workers are not individually pinned. The invoked "
                          "program must not override affinity or NUMA policy. No other benchmark jobs are spawned "
                          "by this runner concurrently; external machine load is not controlled.",
                "jobs": jobs}
    save_manifest(output, manifest)
    for job in jobs:
        started = time.monotonic()
        job.update(status="running", started_utc=stamp())
        save_manifest(output, manifest)
        print(f"START {job['threads']}t on node {node}; log {output / (str(job['threads'])+'t.console.log')}", flush=True)
        try:
            current_rows, current_affinity, current_memory, _ = discover(lscpu)
            current = {row.cpu: row for row in current_rows}
            if job["node"] not in current_memory:
                raise RuntimeError("selected memory node became unavailable")
            for row in job["physical_cores"]:
                cpu = row["cpu"]
                if cpu not in current_affinity or current.get(cpu) != CPU(**row):
                    raise RuntimeError("CPU topology/affinity changed after preflight")
            job["sysfs_before_run"] = verify_sysfs([CPU(**row) for row in job["physical_cores"]])
            expected_siblings = topology["selected_sysfs"]["thread_siblings_list"]
            if any(expected_siblings[cpu] != siblings for cpu, siblings in
                   job["sysfs_before_run"]["thread_siblings_list"].items()):
                raise RuntimeError("physical-core sibling topology changed after preflight")
            for filename, expected in job["command_files_sha256"].items():
                if digest(Path(filename)) != expected:
                    raise RuntimeError(f"command file changed after preflight: {filename}")
            code = run_job(job, output, cwd, numactl, lambda: save_manifest(output, manifest))
            job["returncode"] = code
            if job["status"] != "interrupted":
                job["status"] = "complete" if code == 0 else "failed"
        except (ValueError, RuntimeError, OSError) as error:
            code = 1
            job.update(status="failed", error=str(error))
        except KeyboardInterrupt:
            code = 130
            job["status"] = "interrupted"
        job.update(finished_utc=stamp(), elapsed_s=time.monotonic() - started)
        if code:
            manifest["status"] = job["status"]
            manifest["finished_utc"] = stamp()
            save_manifest(output, manifest)
            print(f"Stopped: {job['threads']}t {job['status']}; see {output/'placement.json'}", file=sys.stderr)
            return code
        save_manifest(output, manifest)
        print(f"Completed {job['threads']}t; console log: {output/job['console_log']}", flush=True)
    manifest.update(status="complete", finished_utc=stamp())
    save_manifest(output, manifest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
