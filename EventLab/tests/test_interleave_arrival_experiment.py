"""Input-oracle, analysis integrity and real-CLI checks; no plotting dependency."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import copy
import csv
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments.interleave_arrival.analyze import analyze
from experiments.common.intervals import interval_curve
from experiments.interleave_arrival.plan import case_name, cases, configuration, validate_plan
from experiments.interleave_arrival.run import run_case
from experiments.interleave_arrival.workload import footprint, records, release_span, write_workload


@contextmanager
def temporary(parent: Path):
    parent = parent.resolve()
    path = parent / ("interleave-test-" + uuid.uuid4().hex)
    path.mkdir()
    try:
        yield path
    finally:
        if path.resolve().parent != parent or path.is_symlink():
            raise RuntimeError("cleanup escaped test parent")
        shutil.rmtree(path)


def check(value, message):
    if not value:
        raise AssertionError(message)


class UnitTests(unittest.TestCase):
    def test_matrix_and_fixed_budgets(self):
        p = validate_plan({})
        matrix = cases(p)
        self.assertEqual(len(matrix), 64)
        self.assertEqual(len({case_name(c) for c in matrix}), 64)
        for c in matrix:
            config = configuration(p, c)
            for text in ("mocs_supported = 1024", "mocs_selected = 1024", "max_pending_write_dlus = 16",
                         "max_open_dlus_per_channel = 16", "send_policy = fcfs", "write_burst = false",
                         "max_timeout_retries = 0", "max_pending_retries = 0", "program_ps = 500000"):
                self.assertIn(text, config)

    def test_reject_invalid_matrix(self):
        for p in ({"typo": 0}, {"channels": True}, {"channels": 16}, {"sizes_bytes": [4096]},
                  {"sizes_bytes": [147456]}, {"interleave_bytes": [0]}, {"interleave_bytes": [96]},
                  {"delta_ps": [-1]}, {"delta_ps": [True]}, {"delta_ps": [1, 1]},
                  {"delta_ps": []}, {"delta_ps": [2**62]}, {"max_tick_ps": 0}):
            with self.subTest(p=p), self.assertRaises(ValueError):
                validate_plan(p)

    def test_global_schedule(self):
        r = records(16384, 50000)
        self.assertEqual(r[0], dict(global_id=1, global_byte=0, scheduled_release=0))
        self.assertEqual(r[-1], dict(global_id=256, global_byte=16320, scheduled_release=12750000))
        self.assertEqual({r["scheduled_release"] for r in records(16384, 0)}, {0})

    def test_invalid_schedule(self):
        for size, delta in ((0, 1), (65, 1), (True, 1), (64, -1), (64, True), (256, 2**62)):
            with self.assertRaises(ValueError):
                records(size, delta)

    def test_mapping_examples(self):
        fine = footprint(16384, 50000, 4, 64)
        coarse = footprint(16384, 50000, 4, 4096)
        a = [r for r in fine if r["channel"] == 0]
        b = [r for r in coarse if r["channel"] == 1]
        self.assertEqual([r["global_byte"] for r in a], list(range(0, 16384, 256)))
        self.assertEqual([r["global_byte"] for r in b], list(range(4096, 8192, 64)))
        self.assertEqual(a[-1]["scheduled_release"]-a[0]["scheduled_release"], 12600000)
        self.assertEqual(b[-1]["scheduled_release"]-b[0]["scheduled_release"], 3150000)

    def test_complete_coverage_and_formula(self):
        for ch in (1, 2, 4, 8):
            for i in (64, 128, 256, 512, 1024, 2048, 4096):
                rows = footprint(ch*8*4096, 12345, ch, i)
                actual = {(r["channel"], r["local_byte"]) for r in rows}
                self.assertEqual(actual, {(c, a) for c in range(ch) for a in range(0, 32768, 64)})
                self.assertEqual(len(actual), len(rows))
        self.assertEqual([release_span(4, i, 1) for i in (64, 256, 1024, 4096)], [252, 243, 207, 63])

    def test_one_channel_control(self):
        self.assertEqual(footprint(32768, 50000, 1, 64), footprint(32768, 50000, 1, 4096))

    def test_invalid_coverage(self):
        for args in ((4096, 1, 4, 64), (16384, 1, 0, 64), (16384, 1, 4, 8192)):
            with self.assertRaises(ValueError):
                footprint(*args)

    def test_trace_identity_and_no_overwrite(self):
        with temporary(Path.cwd()) as path:
            a, b = path / "a.txt", path / "b.txt"
            write_workload(a, 16384, 50000)
            write_workload(b, 16384, 50000)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(len(a.read_text().splitlines()), 257)
            self.assertNotIn(b"\r", a.read_bytes())
            with self.assertRaises(FileExistsError):
                write_workload(a, 16384, 0)

    def test_intervals_and_censoring(self):
        curve, area = interval_curve([(1, 5), (5, 9), (5, 5)], 10)
        self.assertEqual((curve, area["area"]), ({0: 0, 1: 1, 5: 1, 9: 0, 10: 0}, 8))
        self.assertEqual(interval_curve([(2, None)], 5)[1]["area"], 3)
        for intervals in ([(-1, 3)], [(2, 1)], [(2, 6)]):
            with self.assertRaises(ValueError):
                interval_curve(intervals, 5)

    def test_old_statistics_rejected(self):
        with self.assertRaises(ValueError):
            analyze({"schema_version": 2}, {}, {}, 0, Path("unused"))


def integration(cli: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    with temporary(output) as root:
        p = validate_plan({})
        runs = {}

        def run(label, size=16384, interleave=64, delta=50000, plan=None):
            settings = plan or p
            c = dict(size_bytes=size, interleave_bytes=interleave, delta_ps=delta)
            trace = root / (label + ".txt")
            write_workload(trace, size, delta)
            directory = root / label
            r = run_case(cli, directory, trace, settings, c, 90)
            check(r["analysis_status"] == "ok", r)
            runs[label] = (r, directory, c, settings)
            return r

        fine = run("fine")
        coarse = run("coarse", interleave=4096)
        dense = run("dense", delta=0)
        dense_coarse = run("dense-coarse", delta=0, interleave=4096)
        check(fine["successful_dlus"] == 0 and fine["timeout_contexts"] == fine["timeout_with_future_input"] == 4, fine)
        check(coarse["successful_dlus"] == dense["successful_dlus"] == dense_coarse["successful_dlus"] == 4, "success controls")
        check(dense["first_ready_ps"] > dense["analytical_release_span_ps"] == 0, "release span is not assembly duration")
        check(dense["mean_busy_banks"] > 1 and 0 <= dense["mean_bank_utilization"] <= 1, "parallel Banks normalized")
        check(run("slow-coarse", interleave=4096, delta=160000)["timeout_contexts"] == 4, "coarse can also timeout")

        full = run("full-failure", size=131072)
        check(full["planned_dlus"] == 32 and full["failed_dlus"] == 32 and full["timeout_contexts"] == 4, full)
        check(full["cancelled_before_release"] > 0 and full["unhanded_fragments"] > 0, "cancelled suffix remains in denominator")
        check(run("full-success", size=131072, interleave=4096)["successful_dlus"] == 32, "full-block extension")

        at_deadline = run("deadline", plan=p | {"assembly_timeout_ps": 12600000})
        before_deadline = run("deadline-minus-one", plan=p | {"assembly_timeout_ps": 12599999})
        check(at_deadline["successful_dlus"] == 4 and before_deadline["failed_dlus"] == 4, "same-tick arrival beats deadline")

        limited = run("limit", delta=0, plan=p | {"max_tick_ps": 10000})
        check(limited["stop_reason"] == "LimitReached" and limited["unfinished_dlus"] == 4 and limited["logical_completion_ps"] is None,
              "limits must remain censored")
        bank = list(csv.DictReader((runs["limit"][1] / "bank.csv").open(newline="")))
        check(bank and all(not r["completed_at"] for r in bank), "no fabricated completion at limit")

        single = validate_plan({"channels": 1, "sizes_bytes": [4096]})
        control1 = run("one-fine", size=4096, plan=single)
        control2 = run("one-coarse", size=4096, interleave=4096, plan=single)
        for k in ("simulation_end_ps", "successful_dlus", "first_ready_ps", "mean_bank_utilization", "analytical_release_span_ps"):
            check(control1[k] == control2[k], f"single-channel interleave must be inert: {k}")
        repeat = run("repeat")
        check({k: v for k, v in repeat.items() if k != "process_wall_seconds"} ==
              {k: v for k, v in fine.items() if k != "process_wall_seconds"}, "deterministic run")

        sample = json.loads((runs["dense"][1] / "statistics.json").read_text())
        for change in ("mapping", "release", "duplicate", "missing_bank", "bank_overlap", "pending_area", "membership"):
            bad = copy.deepcopy(sample)
            if change == "mapping": bad["logical_work"][0]["channel"] = 1
            elif change == "release": bad["globals"][0]["scheduled_release"] = 99
            elif change == "duplicate": bad["attempts"].append(bad["attempts"][0])
            elif change == "missing_bank": del bad["bank_operations"]
            elif change == "bank_overlap":
                op = copy.deepcopy(bad["bank_operations"][0]); op["ticket"] = 9999; bad["bank_operations"].append(op)
            elif change == "pending_area": bad["pending_channels"][0]["area_context_ticks"]["total"] += 1
            else: bad["dlu_contexts"][0]["members"].pop()
            try:
                analyze(bad, p, runs["dense"][2], 0, runs["dense"][1])
            except ValueError:
                pass
            else:
                raise AssertionError(f"corrupt observations accepted: {change}")

        target = root / "public-run"
        cmd = [sys.executable, str(ROOT / "experiments/interleave_arrival/run.py"), "--cli", str(cli),
               "--output", str(target), "--plan", str(ROOT / "experiments/interleave_arrival/plans/smoke.json"), "--max-cases", "3"]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, timeout=90)
        manifest = json.loads((target / "manifest.json").read_text())
        check(manifest["finished_cases"] == 3 and not manifest["scan_complete"], "partial scan is marked incomplete")
        hashes = [json.loads(f.read_text())["trace_sha256"] for f in sorted((target / "cases").glob("*/result.json"))]
        check(len(set(hashes)) == 2, "different interleaves share identical input files")
        check(subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30).returncode != 0,
              "must reject output overwrite")
    print("PASS interleave/arrival integration: mapping, release, deadlines, cancellation, limits, parallelism, controls, integrity")


if __name__ == "__main__":
    if "--cli" in sys.argv:
        parser = argparse.ArgumentParser()
        parser.add_argument("--cli", required=True, type=Path)
        parser.add_argument("--output", required=True, type=Path)
        args = parser.parse_args()
        integration(args.cli.resolve(), args.output.resolve())
    else:
        unittest.main()
