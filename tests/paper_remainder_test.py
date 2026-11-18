#!/usr/bin/env python3
"""Remainder-selection fixtures; never launch workloads or contact a server."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
# The scripts import each other when run directly from their own directory.
sys.path.insert(0, str(ROOT / "scripts"))
from paper_experiments import CASES

spec = importlib.util.spec_from_file_location("paper_remainder", ROOT / "scripts/paper_remainder.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


class RemainderTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.previous = Path(self.directory.name)
        self.names = list(CASES)

    def manifest(self, threads, jobs, status="running"):
        directory = self.previous / f"{threads}t"
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "manifest.json"
        path.write_text(json.dumps({"threads": threads, "status": status, "jobs": jobs}))
        return path

    def test_completed_and_started_cases_are_never_repeated(self):
        statuses = ("complete", "partial", "failed", "running")
        jobs = [{"name": name, "status": status}
                for name, status in zip(self.names, statuses)]
        self.manifest(16, jobs, status="failed")
        self.assertEqual(module.remaining_cases(self.previous, 16), self.names[len(statuses):])

    def test_each_started_status_is_skipped_even_without_completion_metadata(self):
        for status in ("complete", "partial", "failed", "running"):
            with self.subTest(status=status):
                self.manifest(16, [{"name": self.names[0], "status": status}])
                self.assertEqual(module.remaining_cases(self.previous, 16), self.names[1:])

    def test_absent_32_thread_run_schedules_all_cases(self):
        self.manifest(16, [{"name": name, "status": "complete"} for name in self.names],
                      status="complete")
        self.assertFalse((self.previous / "32t").exists())
        self.assertEqual(module.remaining_cases(self.previous, 32), self.names)
        self.assertFalse((self.previous / "32t").exists())

    def test_pending_cases_are_included_in_canonical_order(self):
        jobs = [{"name": name, "status": "pending"} for name in reversed(self.names)]
        self.manifest(16, jobs)
        self.assertEqual(module.remaining_cases(self.previous, 16), self.names)

    def test_mixed_manifest_preserves_cases_order_not_job_order(self):
        skipped = {self.names[1], self.names[-1]}
        jobs = [{"name": self.names[-1], "status": "running"},
                {"name": self.names[3], "status": "pending"},
                {"name": self.names[1], "status": "complete"},
                {"name": self.names[0], "status": "pending"}]
        self.manifest(16, jobs)
        expected = [name for name in self.names if name not in skipped]
        self.assertEqual(module.remaining_cases(self.previous, 16), expected)
        self.assertEqual(module.remaining_cases(self.previous, 16), expected)

    def test_completed_series_has_no_remainder(self):
        self.manifest(16, [{"name": name, "status": "complete"} for name in self.names],
                      status="complete")
        self.assertEqual(module.remaining_cases(self.previous, 16), [])

    def test_unknown_job_status_fails_closed(self):
        for status in ("unexpected", "", None, "completed"):
            with self.subTest(status=status):
                self.manifest(16, [{"name": self.names[0], "status": status}])
                with self.assertRaises(ValueError):
                    module.remaining_cases(self.previous, 16)

    def test_launch_only_after_stop_and_recheck_manifests(self):
        self.previous = self.previous / "previous"
        first_cases = self.names[:4]
        self.manifest(16, [{"name": n, "status": "complete"} for n in first_cases])
        groups = [{"threads": t, "command_files_sha256": {}, "command": [
            "numactl", "--physcpubind=0", "--membind=0", sys.executable,
            "scripts/paper_experiments.py", "--binary", "build/server-release/adaptive_lock_benchmark",
            "--threads", str(t), "--output", str(self.previous / f"{t}t"), "--repetitions", "3"
        ]} for t in (16, 32)]
        (self.previous / "placement.json").write_text(json.dumps({
            "cwd": str(ROOT), "node": 0, "allow_smt": True, "jobs": groups
        }))
        def finish_old(_):
            # A fifth case had started between the preview and the stop.
            path = self.previous / "16t/manifest.json"
            state = json.loads(path.read_text())
            state["jobs"].append({"name": self.names[4], "status": "failed"})
            path.write_text(json.dumps(state))
        with patch.object(sys, "argv", ["paper_remainder.py", str(self.previous), "--execute"]), \
             patch.object(module, "stop_previous", side_effect=finish_old), \
             patch.object(module.subprocess, "run") as launch:
            module.main()
        self.assertEqual(launch.call_count, 2)
        for call, expected in zip(launch.call_args_list, [self.names[5:], self.names]):
            args = call.args[0]
            self.assertEqual(args[args.index("--repetitions") + 1], "1")
            self.assertEqual(args[args.index("--cases") + 1:], expected)
            self.assertIn("--allow-smt", args)

    @unittest.skipUnless(sys.platform.startswith("linux") and hasattr(os, "pidfd_open"), "Linux pidfd")
    def test_stop_original_native_process_and_wait_for_supervisor(self):
        # Isolated fixture with the same process session layout as the server.
        directory = self.previous / "16t"
        directory.mkdir()
        binary = self.previous / "adaptive_lock_benchmark"
        shutil.copy2(sys.executable, binary)
        native_args = [str(binary), "-c", "import time; time.sleep(30)",
                       "--csv", str(directory / "islands_range.csv")]
        supervisor = (
            "import subprocess,sys,json,pathlib; root=pathlib.Path(sys.argv[1]); "
            "args=sys.argv[2:]; p=subprocess.Popen(args); "
            "tmp=root/'16t/manifest.tmp'; tmp.write_text(json.dumps({'jobs':[{'name':'islands_range',"
            "'status':'running','pid':p.pid,'command':args}]})); "
            "tmp.replace(root/'16t/manifest.json'); "
            "code=p.wait(); sys.exit(1 if code==-15 else 2)"
        )
        with subprocess.Popen([sys.executable, "-c", supervisor, str(self.previous), *native_args],
                              start_new_session=True) as proc:
            original_read = module.read
            placement_path = self.previous / "placement.json"
            def read(path):
                if path == placement_path:
                    return {"status": "running" if proc.poll() is None else "failed",
                            "jobs": [{"status":"running","pid":proc.pid,"threads":16}]}
                return original_read(path)
            with patch.object(module, "read", side_effect=read):
                module.stop_previous(self.previous)
            self.assertEqual(proc.returncode, 1)


if __name__ == "__main__":
    unittest.main()
