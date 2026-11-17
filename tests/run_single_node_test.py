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

    def test_16_and_32_on_one_node_require_explicit_smt(self):
        rows = topology(16)
        affinity = {row.cpu for row in rows}
        with self.assertRaises(ValueError):
            module.select_node(rows, affinity, {0, 1}, [16, 32])
        node, selected = module.select_node(rows, affinity, {0, 1}, [16, 32],
                                             allow_smt=True)
        self.assertEqual(node, 0)
        self.assertEqual([row.cpu for row in selected[16]], list(range(0, 32, 2)))
        self.assertEqual([row.cpu for row in selected[32]],
                         list(range(0, 32, 2)) + list(range(32, 64, 2)))
        self.assertEqual(selected[16], selected[32][:16])
        self.assertEqual(len({(row.socket, row.core) for row in selected[32]}), 16)
        self.assertEqual(len({row.cpu for row in selected[32]}), 32)
        self.assertTrue(all(row.node == node for row in selected[32]))
        # Input order and requested count order must not change nested placement.
        _, reversed_selected = module.select_node(list(reversed(rows)), affinity,
                                                   {0, 1}, [32, 16], allow_smt=True)
        self.assertEqual(reversed_selected, selected)

    def test_smt_uses_every_available_core_before_extra_siblings(self):
        rows = topology(16)
        # CPU0 is excluded but its sibling32 is permitted. CPU34 is excluded,
        # while its core's representative2 remains permitted.
        affinity = {row.cpu for row in rows if row.node == 0} - {0, 34}
        node, selected = module.select_node(rows, affinity, {0}, [16, 30],
                                             allow_smt=True)
        self.assertEqual(node, 0)
        self.assertEqual(len({(row.socket, row.core) for row in selected[16]}), 16)
        self.assertIn(32, {row.cpu for row in selected[16]})
        self.assertEqual({row.cpu for row in selected[30]}, affinity)
        self.assertEqual(selected[16], selected[30][:16])
        with self.assertRaises(ValueError):
            module.select_node(rows, affinity, {0}, [16, 31], allow_smt=True)

    def test_smt_rejects_cross_node_capacity_and_respects_offline(self):
        rows = topology(16)
        affinity = {row.cpu for row in rows}
        # There are64 logical CPUs overall, but only32 on either node.
        with self.assertRaises(ValueError):
            module.select_node(rows, affinity, {0, 1}, [33], allow_smt=True)
        with self.assertRaisesRegex(ValueError, "Mems_allowed_list"):
            module.select_node(rows, affinity, {1}, [16, 32], "0", allow_smt=True)
        rows = [module.CPU(row.cpu, row.core, row.socket, row.node,
                           row.online and row.cpu != 0) for row in rows]
        node, selected = module.select_node(rows, affinity, {0, 1}, [16, 32],
                                             allow_smt=True)
        self.assertEqual(node, 1)
        self.assertTrue(all(row.online and row.node == 1 for row in selected[32]))
        with self.assertRaises(ValueError):
            module.select_node(rows, affinity, {0, 1}, [32], "0", allow_smt=True)
        with self.assertRaises(ValueError):
            module.select_node(rows, affinity, {0}, [32], allow_smt=True)

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

    def test_kernel_smt_masks_and_physical_identity_must_agree(self):
        with tempfile.TemporaryDirectory() as directory:
            system = Path(directory)

            def put(path, value):
                target = system / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(value)

            def masks(first='0,8\n', second='0,8\n', third='2,10\n'):
                put('cpu/cpu0/topology/thread_siblings_list', first)
                put('cpu/cpu8/topology/thread_siblings_list', second)
                put('cpu/cpu2/topology/thread_siblings_list', third)

            put('cpu/online', '0,2,8,10\n')
            put('node/node0/cpulist', '0,2,8,10\n')
            masks()
            selected = [module.CPU(0, 0, 0, 0, True),
                        module.CPU(2, 1, 0, 0, True),
                        module.CPU(8, 0, 0, 0, True)]
            with self.assertRaises(ValueError):
                module.verify_sysfs(selected, system)
            self.assertEqual(module.verify_sysfs(selected, system, allow_smt=True)['node'], 0)
            # Equal core IDs on DIFFERENT sockets are distinct physical keys.
            fake_sibling = module.CPU(8, 0, 1, 0, True)
            with self.assertRaises(ValueError):
                module.verify_sysfs([selected[0], fake_sibling], system, allow_smt=True)
            fake_sibling = module.CPU(8, 2, 0, 0, True)
            with self.assertRaises(ValueError):
                module.verify_sysfs([selected[0], fake_sibling], system, allow_smt=True)
            # A shared lscpu physical identity cannot have disjoint sysfs masks.
            masks(second='8,10\n')
            with self.assertRaises(ValueError):
                module.verify_sysfs([selected[0], selected[2]], system, allow_smt=True)
            # Even mutually overlapping masks must match exactly for siblings.
            masks(second='0,8,10\n')
            with self.assertRaises(ValueError):
                module.verify_sysfs([selected[0], selected[2]], system, allow_smt=True)
            # A selected CPU must occur in its own kernel mask.
            masks(second='0\n')
            with self.assertRaises(ValueError):
                module.verify_sysfs([selected[0], selected[2]], system, allow_smt=True)
            masks()
            with self.assertRaises(ValueError):
                module.verify_sysfs([selected[0], selected[0]], system, allow_smt=True)
            put('cpu/online', '0,2,10\n')
            with self.assertRaises(ValueError):
                module.verify_sysfs(selected, system, allow_smt=True)
            put('cpu/online', '0,2,8,10\n')
            put('node/node0/cpulist', '0,2,10\n')
            with self.assertRaises(ValueError):
                module.verify_sysfs(selected, system, allow_smt=True)

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

    def test_smt_job_metadata_distinguishes_logical_cpus_from_physical_cores(self):
        rows = topology(16)
        node, selected = module.select_node(rows, {row.cpu for row in rows}, {0},
                                             [16, 32], allow_smt=True)
        with tempfile.TemporaryDirectory() as directory:
            cwd = Path(directory)
            jobs = module.build_jobs([sys.executable, '-c', 'pass', '--threads', '{threads}',
                                      '--output', '{output}'], cwd / 'results', cwd,
                                     '/usr/bin/numactl', node, selected)
        self.assertEqual([job['logical_cpu_count'] for job in jobs], [16, 32])
        self.assertEqual([job['physical_core_count'] for job in jobs], [16, 16])
        self.assertEqual([job['smt_siblings_used'] for job in jobs], [False, True])
        for job in jobs:
            self.assertEqual(job['node'], 0)
            self.assertEqual(len(job['cpu_topology']), job['logical_cpu_count'])
            self.assertEqual({row['node'] for row in job['cpu_topology']}, {0})
            self.assertEqual(len({(row['socket'], row['core']) for row in job['cpu_topology']}), 16)
            self.assertEqual(module.parse_id_list(job['binding_command'][1].split('=', 1)[1]),
                             set(job['cpus']))
            self.assertEqual(job['binding_command'][2], '--membind=0')
        self.assertEqual(jobs[0]['cpus'], jobs[1]['cpus'][:16])
        self.assertNotEqual(jobs[0]['output_placeholder'], jobs[1]['output_placeholder'])

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

    def test_explicit_smt_dry_run_passes_policy_to_kernel_verification(self):
        rows = topology(16)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'not-created'
            with mock.patch.object(module.platform, 'system', return_value='Linux'), \
                 mock.patch.object(module.os, 'sched_getaffinity', create=True), \
                 mock.patch.object(module.shutil, 'which', side_effect=lambda s: '/usr/bin/'+s), \
                 mock.patch.object(module, 'discover', return_value=(rows, {r.cpu for r in rows}, {0, 1}, {})), \
                 mock.patch.object(module, 'verify_sysfs', return_value={}) as verify, \
                 mock.patch.object(module.subprocess, 'Popen') as launch, \
                 contextlib.redirect_stdout(io.StringIO()) as printed:
                result = module.main(['--threads', '16', '32', '--node', '0', '--allow-smt',
                                      '--output', str(output), '--', sys.executable, '-c', 'pass',
                                      '--threads', '{threads}', '--output', '{output}'])
            self.assertEqual(result, 0)
            self.assertFalse(output.exists())
            launch.assert_not_called()
            verify.assert_called_once()
            self.assertTrue(verify.call_args.kwargs['allow_smt'])
            selected = verify.call_args.args[0]
            self.assertEqual(len(selected), 32)
            self.assertEqual({row.node for row in selected}, {0})
            self.assertEqual(len({(row.socket, row.core) for row in selected}), 16)
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
