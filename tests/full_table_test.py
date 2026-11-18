#!/usr/bin/env python3
"""Placement and reuse regressions for the complete submitted workload table."""
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import full_table as suite
from run_single_node import CPU


class TableTests(unittest.TestCase):
    def topology(self):
        return [CPU(core + lane * 64, core, core // 32, core // 16, True)
                for core in range(64) for lane in (0, 1)]

    def test_requested_physical_and_smt_placements(self):
        rows = self.topology()
        profiles = suite.placements(rows, set(range(128)), set(range(4)))
        expected = {"16t": list(range(16)), "32t-smt": list(range(16)) + list(range(64, 80)),
                    "32t-physical": list(range(32)), "64t-physical": list(range(64))}
        for name, cpus in expected.items():
            self.assertEqual(profiles[name]["cpus"], cpus)
            self.assertEqual(profiles[name]["threads"], len(cpus))
        self.assertEqual(profiles["32t-smt"]["physical_cores"], 16)
        self.assertEqual(profiles["32t-physical"]["physical_cores"], 32)
        self.assertEqual(profiles["64t-physical"]["physical_cores"], 64)
        self.assertEqual(suite.binding(profiles["64t-physical"], "numactl")[-1], "--interleave=0,1,2,3")
        for cpus, nodes in [(set(range(64)), set(range(4))), (set(range(128)), {0, 1, 2})]:
            with self.assertRaises(ValueError):
                suite.placements(rows, cpus, nodes)

    def test_every_submitted_workload(self):
        self.assertEqual(len(suite.CASES), 18)
        self.assertEqual(len({c[0] for c in suite.CASES}), 18)
        self.assertEqual([c[0] for c in suite.CASES if c[-1]],
                         ["steady_short", "clusters_2_short", "moving_short"])
        self.assertEqual(suite.POLICIES, ("fixed", "balanced", "dynamic_dp", "global", "per_element"))

    def test_reuse_complete_triples_and_preserve_partial_csv(self):
        case = suite.CASES[0]
        profile = {"name": "16t", "threads": 16, "cpus": list(range(16))}
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "16t").mkdir()
            (root / "placement.json").write_text(json.dumps({"node": 0, "jobs": [profile]}))
            path = root / "16t/uniform.csv"
            rows = [dict(implementation=p, repetition="1", verified="true", scenario="uniform",
                         threads="16", operations="5000000000", model="paper", write_body="range",
                         lock_kind="mutex", range_length="1", read_percent="0", point_percent="0",
                         array_size=str(2**20), hot_width=str(2**14), bins="1024", sample_rate="64",
                         **{"lambda": "10000"}) for p in suite.POLICIES[:3]]
            rows.append(dict(rows[0], repetition="2"))
            with path.open("w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            before = path.read_bytes()
            reuse = suite.old_rows(case, profile, root)
            self.assertEqual(set(reuse), set(suite.POLICIES[:3]))
            self.assertTrue(all(len(j["results"]) == 1 for j in reuse.values()))
            self.assertEqual(path.read_bytes(), before)
            profile["cpus"] = list(range(16, 32))
            with self.assertRaises(ValueError):
                suite.old_rows(case, profile, root)


if __name__ == "__main__":
    unittest.main()
