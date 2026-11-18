#!/usr/bin/env python3
"""Placement and reuse regressions for the complete submitted workload table."""
import csv
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import full_table as suite
from run_single_node import CPU


class TableTests(unittest.TestCase):
    def mixed_manifest(self):
        policies = ("fixed", "balanced", "dynamic_dp", "mgl64", "kdi_list_ex")
        factors = {"fixed": 1, "balanced": 2, "dynamic_dp": 1.5, "mgl64": 0.8,
                   "kdi_list_ex": 0.5}
        manifest = {"status": "complete", "mixed_only": True, "short_only": False,
                    "length_sweep": False, "baseline": None,
                    "profiles": {name: {} for name in suite.PROFILES}, "jobs": []}
        for profile_index, profile in enumerate(suite.PROFILES):
            for case_index, (case, points) in enumerate(suite.MIXED_POINTS.items()):
                count = 12_345_678 if case_index == 0 else 1234
                seconds = 10 + profile_index * 2 + case_index
                for policy in policies:
                    manifest["jobs"].append({
                        "profile": profile, "case": case, "implementation": policy,
                        "status": "complete", "method": "timed", "seconds": seconds,
                        "source": f"{profile}/{case}/{policy}.csv",
                        "results": [{"operations": str(count), "wall_s": str(10 / factors[policy]),
                                     "point_percent": str(points), "rebuilds": "0"}],
                    })
        return manifest

    def test_extend_mixed_adds_missing_baselines_and_preserves_results(self):
        manifest = self.mixed_manifest()
        before = copy.deepcopy(manifest["jobs"])
        suite.extend_mixed_baselines(manifest)
        indexed = {(j["profile"], j["case"], j["implementation"]): j for j in manifest["jobs"]}
        self.assertEqual(len(manifest["jobs"]), 56)
        self.assertEqual(len(indexed), 56)
        for job in before:
            key = job["profile"], job["case"], job["implementation"]
            self.assertEqual(indexed[key], job)
        for profile in suite.PROFILES:
            for case in suite.MIXED_POINTS:
                fixed = indexed[profile, case, "fixed"]
                for policy in ("global", "per_element"):
                    added = indexed[profile, case, policy]
                    self.assertEqual(added["seconds"], fixed["seconds"])
                    self.assertEqual(added["status"], "pending")
                    self.assertEqual(added["results"], [])
        extended = copy.deepcopy(manifest)
        suite.extend_mixed_baselines(manifest)
        self.assertEqual(manifest, extended)

    def test_extend_mixed_keeps_existing_baseline_measurement(self):
        manifest = self.mixed_manifest()
        existing = dict(copy.deepcopy(manifest["jobs"][0]), implementation="global",
                        seconds=77, source="existing-global.csv")
        manifest["jobs"].append(existing)
        saved = copy.deepcopy(existing)
        suite.extend_mixed_baselines(manifest)
        matches = [j for j in manifest["jobs"] if (j["profile"], j["case"], j["implementation"]) ==
                   (existing["profile"], existing["case"], "global")]
        self.assertEqual(matches, [saved])
        self.assertEqual(len(manifest["jobs"]), 56)

    def test_mixed_report_has_actual_fixed_q_and_requested_baselines(self):
        manifest = self.mixed_manifest()
        suite.extend_mixed_baselines(manifest)
        for job in manifest["jobs"]:
            if job["status"] == "pending":
                factor = 0.25 if job["implementation"] == "global" else 4
                count = 12_345_678 if job["case"] == "mixed_90p_10r" else 1234
                job.update(status="complete", results=[{"operations": str(count),
                                                         "wall_s": str(10 / factor), "rebuilds": "0"}])
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            suite.report(directory, manifest)
            text = (directory / "TABLE.md").read_text()
            self.assertNotIn("Kogan", text)
            self.assertNotIn("kdi_list_ex", text)
            headers = [line for line in text.splitlines() if line.startswith("| Workload |")]
            self.assertEqual(len(headers), 4)
            for header in headers:
                self.assertIn("| Q fixed (M) |", header)
                self.assertIn("| Global |", header)
                self.assertIn("| Element |", header)
                self.assertIn("| MGL64 |", header)
            rows = [[cell.strip() for cell in line.strip("|").split("|")]
                    for line in text.splitlines() if line.startswith(("| 90% points", "| 10% points"))]
            self.assertEqual(len(rows), 8)
            self.assertEqual([row[1] for row in rows], ["12.3", "0.00123"] * 4)
            persisted = json.loads((directory / "manifest.json").read_text())
            kdi = [j for j in persisted["jobs"] if j["implementation"] == "kdi_list_ex"]
            self.assertEqual(len(kdi), 8)
            self.assertTrue(all(j["results"] and j["status"] == "complete" for j in kdi))

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
