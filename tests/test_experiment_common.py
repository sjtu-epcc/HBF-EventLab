"""Shared infrastructure contracts; no CLI or plotting dependency."""
import ast
import json
import os
from pathlib import Path
import sys
import shutil
import uuid
from contextlib import contextmanager
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments.common.config import load_run_configuration, parse_configuration, scalar
from experiments.common.intervals import interval_curve
from experiments.common.io import csv_rows, digest, read_csv, write_json
from experiments.common.provenance import script_hashes
from experiments.common.model import PAGE_BYTES
from experiments.arrival_policy.workload import records as arrival_records
from experiments.read_scheduling.workload import records as read_records


@contextmanager
def temporary_directory():
    # Inherit parent ACLs like the other Windows experiment tests.
    parent = Path.cwd().resolve()
    path = parent / ("experiment-common-" + uuid.uuid4().hex)
    path.mkdir()
    try:
        yield path
    finally:
        if path.is_symlink() or path.resolve().parent != parent:
            raise RuntimeError("cleanup escaped test directory")
        shutil.rmtree(path)


REQUEST = """[device.geometry]
host_channels = 1
dies_per_channel = 1
banks_per_die = 1
pages_per_block = 12
[device.capabilities]
mocs_supported = 1024
max_pending_write_dlus = 7
[device.selected]
mocs_selected = 512
zone_size_dlus = auto
[host]
send_policy = fcfs
"""
ACTUAL = REQUEST.replace("zone_size_dlus = auto", "zone_size_dlus = 12").replace(
    "send_policy = fcfs", 'send_policy = "fcfs"') + '\n[device]\nspec_profile = "ocp_hbf_v0.7.0"\n'


class CommonTests(unittest.TestCase):
    def test_repeated_sections_but_not_keys(self):
        self.assertEqual(parse_configuration("[a]\nx=1\n[b]\ny=2\n[a]\nz=3"),
                         {"a.x": "1", "b.y": "2", "a.z": "3"})
        for bad in ("[a]\nx=1\n[a]\nx=2", "x=1", "[]\nx=1", "[a]\nx", "[a]\nx="):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                parse_configuration(bad)

    def test_scalar_comparison(self):
        self.assertEqual(scalar('"fcfs"'), scalar("fcfs"))
        self.assertEqual(scalar("0x100"), scalar("256"))
        self.assertIs(scalar("false"), False)

    def test_actual_configuration_drives_geometry_and_budgets(self):
        with temporary_directory() as tmp:
            p = Path(tmp)
            (p / "input.ini").write_text(REQUEST, encoding="utf-8")
            (p / "effective.ini").write_text(ACTUAL, encoding="utf-8")
            cfg = load_run_configuration(p)
            self.assertEqual((cfg.pages_per_block, cfg.selected_mocs, cfg.pending_limit), (12, 512, 7))
            self.assertEqual(arrival_records("batched", cfg.pages_per_block)[32]["address"], 12 * PAGE_BYTES)
            self.assertEqual(read_records("competition", cfg.pages_per_block)[1]["address"], 12 * PAGE_BYTES)

    def test_requested_configuration_must_take_effect(self):
        with temporary_directory() as tmp:
            p = Path(tmp)
            (p / "input.ini").write_text(REQUEST, encoding="utf-8")
            for bad in (ACTUAL.replace("mocs_selected = 512", "mocs_selected = 256"),
                        ACTUAL.replace("zone_size_dlus = 12", "zone_size_dlus = 8"),
                        ACTUAL.replace("ocp_hbf_v0.7.0", "unknown"),
                        ACTUAL.replace("max_pending_write_dlus = 7", "max_pending_write_dlus = 16")):
                (p / "effective.ini").write_text(bad, encoding="utf-8")
                with self.assertRaises(ValueError):
                    load_run_configuration(p)
            (p / "effective.ini").unlink()
            with self.assertRaises(FileNotFoundError):
                load_run_configuration(p)

    def test_interval_boundaries_and_censoring(self):
        curve, summary = interval_curve([(1, 5), (5, 9), (5, 5), (9, None)], 10)
        self.assertEqual(curve, {0: 0, 1: 1, 5: 1, 9: 1, 10: 1})
        self.assertEqual(summary, dict(peak_tick_boundary=1, area=9, mean=.9, busy_ticks=9, current=1))
        self.assertIsNone(interval_curve([], 0)[1]["mean"])
        for intervals, stop in (([], -1), ([], True), ([(0, 2.5)], 3), ([(False, 1)], 2), ([(2, 1)], 3)):
            with self.assertRaises(ValueError):
                interval_curve(intervals, stop)

    def test_utf8_outputs_and_hashes(self):
        with temporary_directory() as tmp:
            p = Path(tmp)
            write_json(p / "a.json", {"内容": "验证"})
            self.assertEqual(json.loads((p / "a.json").read_text(encoding="utf-8")), {"内容": "验证"})
            self.assertEqual(len(digest(p / "a.json")), 64)
            csv_rows(p / "a.csv", [{"name": "验证", "n": 2}])
            self.assertEqual(read_csv(p / "a.csv"), [{"name": "验证", "n": "2"}])
            with self.assertRaises(ValueError):
                write_json(p / "bad.json", float("nan"))

    def test_no_cross_experiment_imports(self):
        for path in (ROOT / "experiments").rglob("*.py"):
            group = path.relative_to(ROOT / "experiments").parts[0]
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
                if isinstance(node, ast.ImportFrom) and node.module and node.module.startswith("experiments."):
                    target = node.module.split(".")[1]
                    self.assertIn(target, {group, "common"}, str(path))

    def test_provenance_includes_shared_sources(self):
        hashes = script_hashes(ROOT / "experiments/read_scheduling")
        self.assertIn("experiments/common/config.py", hashes)
        self.assertIn("experiments/read_scheduling/run.py", hashes)
        self.assertFalse(any("interleave_arrival" in k for k in hashes))
        relative = Path(os.path.relpath(ROOT / "experiments/read_scheduling", Path.cwd()))
        self.assertEqual(script_hashes(relative), hashes)


if __name__ == "__main__":
    unittest.main()
