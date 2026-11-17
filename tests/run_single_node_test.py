#!/usr/bin/env python3
"""Pure topology/command fixture tests; no server or benchmark execution."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import contextlib
import io

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("run_single_node", ROOT / "scripts/run_single_node.py")
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def topology(cores_per_node=64):
    # NUMA CPU lists are interleaved, with SMT siblings in another ID range.
    rows = []
    total_cores = 2 * cores_per_node
    for node in (0, 1):
        for core in range(cores_per_node):
            for sibling in (0, 1):
                rows.append(module.CPU(2 * core + node + sibling * total_cores,
                                       core, node, node, True))
    return rows


class PlacementTests(unittest.TestCase):
    def test_linux_id_lists(self):
        self.assertEqual(module.parse_id_list("0-3,8,10-12"), {0, 1, 2, 3, 8, 10, 11, 12})
        self.assertEqual(module.parse_id_list(""), set())
        for invalid in ("3-1", "-1", "0,,2", "1;echo", "1-x"):
            with self.assertRaises(ValueError): module.parse_id_list(invalid)

    def test_csv_online_and_missing_offline_identity(self):
        rows = module.parse_lscpu("# CPU,Core,Socket,Node,Online\n0,0,0,1,Y\n8,0,0,1,Y\n9,-,-,-,N\n")
        self.assertEqual(rows[0], module.CPU(0, 0, 0, 1, True))
        self.assertFalse(rows[-1].online)
        for invalid in ("0,0,0,-,Y", "0,0,0,0,?", "0,0,0,0,Y\n0,0,0,0,Y", "0,0,0,0,Y\n1,0,0,1,Y"):
            with self.assertRaises(ValueError): module.parse_lscpu(invalid)

    def test_128_physical_cores_two_nodes(self):
        rows = topology()
        node, selected = module.select_node(rows, {r.cpu for r in rows}, {0, 1}, [32, 64])
        self.assertEqual(node, 0)
        self.assertEqual([r.cpu for r in selected[32]], list(range(0, 64, 2)))
        self.assertEqual([r.cpu for r in selected[64]], list(range(0, 128, 2)))
        self.assertEqual(selected[32], selected[64][:32])
        self.assertEqual(len({(r.socket, r.core) for r in selected[64]}), 64)
        self.assertTrue(all(r.node == 0 for r in selected[64]))

    def test_128_logical_cpus_are_not_128_cores(self):
        rows = topology(32)
        affinity = {r.cpu for r in rows}
        self.assertEqual(len(affinity), 128)
        with self.assertRaisesRegex(ValueError, "64 permitted physical cores"):
            module.select_node(rows, affinity, {0, 1}, [32, 64])
        node, selected = module.select_node(rows, affinity, {0, 1}, [32])
        self.assertEqual(node, 0)
        self.assertEqual(len(selected[32]), 32)

    def test_cpuset_and_memory_nodes(self):
        rows = topology()
        only_second_siblings = {r.cpu for r in rows if r.cpu >= 128}
        node, selected = module.select_node(rows, only_second_siblings, {1}, [32, 64])
        self.assertEqual(node, 1)
        self.assertTrue(all(r.cpu >= 128 and r.node == 1 for r in selected[64]))
        with self.assertRaisesRegex(ValueError, "Mems_allowed_list"):
            module.select_node(rows, only_second_siblings, {1}, [32], "0")
        with self.assertRaises(ValueError):
            module.select_node(rows, {r.cpu for r in rows if r.core < 16}, {0, 1}, [32])

    def test_offline_cpu_and_fallback_node(self):
        rows = topology()
        # Both SMT siblings of node0/core0 are offline; auto must use node1 for64.
        rows = [module.CPU(r.cpu, r.core, r.socket, r.node,
                           not (r.node == 0 and r.core == 0)) for r in rows]
        node, selected = module.select_node(rows, {r.cpu for r in rows}, {0, 1}, [32, 64])
        self.assertEqual(node, 1)
        with self.assertRaisesRegex(ValueError, "63 permitted physical cores"):
            module.select_node(rows, {r.cpu for r in rows}, {0, 1}, [64], "0")
        self.assertTrue(all(r.online for r in selected[64]))

    def test_invalid_counts(self):
        rows = topology()
        for counts in ([], [0], [-1], [32, 32]):
            with self.assertRaises(ValueError): module.select_node(rows, {r.cpu for r in rows}, {0, 1}, counts)

    def test_effective_policy_verification(self):
        output = "policy: bind\npreferred node: 0\nphyscpubind: 0 2 4\ncpubind: 0\nnodebind: 0\nmembind: 0\n"
        module.parse_numactl_show(output, [0, 2, 4], 0)
        with self.assertRaises(ValueError): module.parse_numactl_show(output.replace("policy: bind", "policy: default"), [0, 2, 4], 0)
        with self.assertRaises(ValueError): module.parse_numactl_show(output.replace("membind: 0", "membind: 0 1"), [0, 2, 4], 0)
        with self.assertRaises(ValueError): module.parse_numactl_show(output, [0, 2], 0)

    def test_kernel_sibling_and_node_crosscheck(self):
        with tempfile.TemporaryDirectory() as directory:
            system = Path(directory)
            def put(path, value):
                target = system / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(value)
            put('cpu/online', '0-3,8-11\n')
            put('node/node0/cpulist', '0,2,8,10\n')
            put('cpu/cpu0/topology/thread_siblings_list', '0,8\n')
            put('cpu/cpu2/topology/thread_siblings_list', '2,10\n')
            selected = [module.CPU(0, 0, 0, 0, True), module.CPU(2, 1, 0, 0, True)]
            self.assertEqual(module.verify_sysfs(selected, system)['node'], 0)
            put('cpu/cpu2/topology/thread_siblings_list', '0,2,8,10\n')
            with self.assertRaisesRegex(ValueError, 'share a physical core'):
                module.verify_sysfs(selected, system)
            put('cpu/cpu2/topology/thread_siblings_list', '2,10\n')
            put('node/node0/cpulist', '0,8\n')
            with self.assertRaisesRegex(ValueError, 'disagrees'):
                module.verify_sysfs(selected, system)

    def test_commands_are_arg_arrays_and_outputs_are_distinct(self):
        rows = topology()
        node, selected = module.select_node(rows, {r.cpu for r in rows}, {0, 1}, [32, 64])
        with tempfile.TemporaryDirectory() as directory:
            cwd = Path(directory)
            jobs = module.build_jobs([sys.executable, "-c", "pass", "--threads", "{threads}",
                                      "--output", "{output}", "literal;$()"],
                                     cwd / "new results", cwd, "/usr/bin/numactl", node, selected)
        self.assertEqual(jobs[0]["command"][-1], "literal;$()")
        self.assertEqual(jobs[0]["command"][2], "--membind=0")
        self.assertIn("32", jobs[0]["command"])
        self.assertIn("64", jobs[1]["command"])
        self.assertNotEqual(jobs[0]["output_placeholder"], jobs[1]["output_placeholder"])
        self.assertEqual(len(jobs[0]["command_files_sha256"]), 1)
        for command in ([sys.executable, "--output", "{output}"], [sys.executable, "--threads", "{threads}"]):
            with self.assertRaises(ValueError): module.build_jobs(command, Path('/tmp/new'), ROOT, 'numactl', node, selected)

    def test_default_dry_run_launches_nothing_and_creates_nothing(self):
        rows = topology()
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'not-created'
            with mock.patch.object(module.platform, 'system', return_value='Linux'), \
                 mock.patch.object(module.os, 'sched_getaffinity', create=True), \
                 mock.patch.object(module.shutil, 'which', side_effect=lambda s: '/usr/bin/'+s), \
                 mock.patch.object(module, 'discover', return_value=(rows, {r.cpu for r in rows}, {0, 1}, {})), \
                 mock.patch.object(module, 'verify_sysfs', return_value={}), \
                 mock.patch.object(module.subprocess, 'Popen') as launch, \
                 contextlib.redirect_stdout(io.StringIO()) as printed:
                result = module.main(['--output', str(output), '--', sys.executable, '-c', 'pass',
                                      '--threads', '{threads}', '--output', '{output}'])
            self.assertEqual(result, 0)
            self.assertFalse(output.exists())
            launch.assert_not_called()
            self.assertIn('DRY RUN', printed.getvalue())

    def test_wrong_effective_policy_prevents_workload_launch(self):
        job = {'threads': 32, 'node': 0, 'cpus': [0, 2], 'command': ['never-run'],
               'binding_command': ['numactl', '--physcpubind=0,2', '--membind=0']}
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(module, 'command_output', return_value='policy: default\n'), \
                 mock.patch.object(module.subprocess, 'Popen') as launch:
                with self.assertRaises(ValueError):
                    module.run_job(job, Path(directory), ROOT, 'numactl', lambda: None)
                launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
