"""Unit and real-CLI checks for the capacity experiment, with no plotting dependency."""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import shutil
import unittest
import uuid
from contextlib import contextmanager

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments.capacity.analyze import analyze, interval_curve, ratio
from experiments.capacity.plan import case_name, cases, configuration, validate_plan
from experiments.capacity.run import run_case
from experiments.capacity.workload import records, validate_coverage, write_workload


@contextmanager
def test_directory(parent: Path):
    # Inherit the build directory's ACL, including when run by a restricted Windows test worker.
    parent = parent.resolve()
    path = parent / ("capacity-test-" + uuid.uuid4().hex)
    path.mkdir()
    try:
        yield path
    finally:
        if path.resolve().parent != parent or path.is_symlink():
            raise RuntimeError("test cleanup target escaped its parent")
        shutil.rmtree(path)


class UnitTests(unittest.TestCase):
    def test_coverage(self):
        a, b = records(8, "dlu_major"), records(8, "fragment_round_robin")
        self.assertEqual(set(a), set(b))
        self.assertNotEqual(a, b)
        self.assertEqual(a[:2], [(1, 0), (2, 64)])
        self.assertEqual(b[:2], [(1, 0), (65, 32768)])
        validate_coverage(a, 8)
        with self.assertRaises(ValueError):
            validate_coverage(a[:-1] + a[:1], 8)

    def test_invalid_workload(self):
        for size in (0, -1, True, 1025):
            with self.assertRaises(ValueError):
                records(size, "dlu_major")
        with self.assertRaises(ValueError):
            records(1, "random")

    def test_deterministic_file(self):
        with test_directory(Path.cwd()) as tmp:
            a, b = Path(tmp) / "a", Path(tmp) / "b"
            write_workload(a, 4, "fragment_round_robin")
            write_workload(b, 4, "fragment_round_robin")
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(len(a.read_text().splitlines()), 257)
            self.assertNotIn(b"\r", a.read_bytes())

    def test_plan(self):
        p = validate_plan({})
        self.assertEqual(len(cases(p)), 648)
        self.assertEqual(len({case_name(c) for c in cases(p)}), 648)
        config = configuration(p, cases(p)[0])
        for text in ("host_channels = 1", "write_burst = false", "write_mode = fragment64", "mocs_supported = 1024",
                     "max_pending_retries = 0", "max_timeout_retries = 0", "send_policy = fcfs"):
            self.assertIn(text, config)

    def test_invalid_plan(self):
        bad = [{"typo": 1}, {"mocs_selected": [64]}, {"mocs_selected": [300]}, {"mocs_selected": [32768]},
               {"open_dlus": [65]}, {"program_ps": [0]}, {"pending_limits": []}, {"orders": ["a"]},
               {"orders": ["dlu_major", "dlu_major"]}, {"dlus": True}, {"pending_limits": [True]},
               {"open_dlus": [1, 1]}, {"max_tick_ps": -1}, {"program_ps": [2**63]}]
        for item in bad:
            with self.subTest(item=item), self.assertRaises(ValueError):
                validate_plan(item)

    def test_same_tick_intervals(self):
        curve, s = interval_curve([(1, 5), (5, 9), (5, 5)], 10)
        self.assertEqual(curve, {0: 0, 1: 1, 5: 1, 9: 0, 10: 0})
        self.assertEqual(s["peak_tick_boundary"], 1)
        self.assertEqual(s["area"], 8)
        self.assertEqual(s["mean"], .8)

    def test_limit_intervals(self):
        curve, s = interval_curve([(2, None), (3, 5)], 5)
        self.assertEqual(s["area"], 5)
        self.assertEqual(s["current"], 1)
        self.assertEqual(curve[5], 1)
        self.assertIsNone(ratio(0, 0))
        self.assertEqual(interval_curve([], 0)[1]["peak_tick_boundary"], 0)

    def test_invalid_intervals(self):
        for xs in ([(5, 2)], [(-1, 2)], [(0, 11)], [(11, None)]):
            with self.assertRaises(ValueError):
                interval_curve(xs, 10)

    def test_missing_bank_schema(self):
        for data in ({"schema_version": 1}, {"schema_version": 2}):
            with self.assertRaises(ValueError):
                analyze(data, 8, 256, 0, Path("unused"))


def integration(cli: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    with test_directory(output) as temp:
        root = Path(temp)
        p = validate_plan(json.loads((ROOT / "experiments/capacity/plans/smoke.json").read_text()))
        traces = {}
        for order in p["orders"]:
            traces[order] = root / f"{order}.txt"
            write_workload(traces[order], p["dlus"], order)
        results = {}
        for case in cases(p):
            row = run_case(cli, root / case_name(case), traces[case["order"]], p, case, 30)
            assert row["analysis_status"] == "ok", row
            results[case_name(case)] = row
            assert row["successful_dlus"] + row["failed_dlus"] == 8, row
            assert row["mocs_peak_tick_boundary"] <= case["mocs_selected"]
            assert row["pending_peak"] <= case["pending_limit"]
        def get(order, opened, mocs):
            return results[case_name(dict(order=order, open_dlus=opened, mocs_selected=mocs, pending_limit=8, program_ps=500000))]
        assert get("dlu_major", 5, 256)["successful_dlus"] == 8
        assert get("fragment_round_robin", 4, 256)["successful_dlus"] == 8
        starving = get("fragment_round_robin", 5, 256)
        assert starving["timeout_contexts"] > 0 and starving["successful_dlus"] < 8, starving
        assert get("fragment_round_robin", 5, 512)["successful_dlus"] == 8
        base = dict(order="fragment_round_robin", open_dlus=5, mocs_selected=512, pending_limit=2, program_ps=500000)
        rejected = run_case(cli, root / "pending-limit", traces[base["order"]], p, base, 30)
        assert rejected["analysis_status"] == "ok" and rejected["pending_limit_responses"] > 0, rejected
        assert rejected["opened_contexts"] < rejected["planned_dlus"], rejected
        limited = run_case(cli, root / "limited", traces[base["order"]], p | {"max_tick_ps": 10000}, base, 30)
        assert limited["analysis_status"] == "ok" and limited["stop_reason"] == "LimitReached", limited
        assert 0 < limited["unfinished_dlus"] < 8 and limited["logical_completion_ps"] is None, limited
        assert limited["failed_dlus"] + limited["unfinished_dlus"] == 8, limited
        # Repeated execution is exactly reproducible in simulated time and logical/resource counts.
        repeat_case = dict(order="fragment_round_robin", open_dlus=5, mocs_selected=256, pending_limit=8, program_ps=500000)
        repeated = run_case(cli, root / "repeat", traces[repeat_case["order"]], p, repeat_case, 30)
        for key in starving:
            if key != "process_wall_seconds":
                assert repeated[key] == starving[key], (key, repeated, starving)
        sample_dir = root / case_name(repeat_case)
        sample = json.loads((sample_dir / "statistics.json").read_text())
        for mutation in ("duplicate", "missing_bank", "overlap"):
            bad = copy.deepcopy(sample)
            if mutation == "duplicate":
                bad["logical_work"].append(bad["logical_work"][0])
            elif mutation == "missing_bank":
                del bad["bank_operations"]
            else:
                bad["bank_operations"].append(bad["bank_operations"][0])
            try:
                analyze(bad, 8, 256, 2, sample_dir)
            except ValueError:
                pass
            else:
                raise AssertionError(f"corrupt observations accepted: {mutation}")
        # Public entry point records partial scan honestly and refuses to overwrite it.
        target = root / "public-run"
        command = [sys.executable, str(ROOT / "experiments/capacity/run.py"), "--cli", str(cli),
                   "--output", str(target), "--max-cases", "1"]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL, timeout=30)
        manifest = json.loads((target / "manifest.json").read_text())
        assert manifest["selected_cases"] == 1 and not manifest["scan_complete"]
        again = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        assert again.returncode != 0
    print("PASS capacity integration: credit boundary, Pending rejection, limits, determinism, integrity, CLI")


if __name__ == "__main__":
    if "--cli" in sys.argv:
        parser = argparse.ArgumentParser()
        parser.add_argument("--cli", type=Path, required=True)
        parser.add_argument("--output", type=Path, required=True)
        args = parser.parse_args()
        integration(args.cli.resolve(), args.output.resolve())
    else:
        unittest.main()
