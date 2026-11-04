"""Finite Host-policy example: fixed input fairness, actual handoff and Assembly checks."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments.arrival_policy.analyze import analyze
from experiments.arrival_policy.run import normalized_effective, verify_comparisons
from experiments.arrival_policy.workload import BLOCK_BYTES, POLICIES, SHAPES, configuration, records, write_workload


@contextmanager
def temporary(parent: Path):
    parent = parent.resolve()
    path = parent / ("arrival-policy-test-" + uuid.uuid4().hex)
    path.mkdir()
    try:
        yield path
    finally:
        if path.resolve().parent != parent or path.is_symlink():
            raise RuntimeError("cleanup escaped test parent")
        shutil.rmtree(path)


class UnitTests(unittest.TestCase):
    def test_two_complete_independent_pages(self):
        for shape in SHAPES:
            rows = records(shape)
            self.assertEqual(len(rows), 128)
            self.assertEqual([r["id"] for r in rows], list(range(1, 129)))
            self.assertEqual({r["address"] for r in rows}, set(range(0, 4096, 64)) | set(range(BLOCK_BYTES, BLOCK_BYTES+4096, 64)))
            self.assertEqual({r["content"] for r in rows}, {1})

    def test_competing_batch(self):
        rows = records("batched")
        self.assertEqual([(r["page"], r["at"]) for r in rows[:32]], [("A", 0)]*32)
        self.assertEqual([(r["page"], r["at"]) for r in rows[32:96]], [("B", 2_000_000)]*32+[("A", 2_000_000)]*32)
        self.assertEqual([(r["page"], r["at"]) for r in rows[96:]], [("B", 4_000_000)]*32)

    def test_sparse_changes_only_at(self):
        a, b = records("batched"), records("sparse")
        self.assertEqual([{k:v for k,v in r.items() if k != "at"} for r in a],
                         [{k:v for k,v in r.items() if k != "at"} for r in b])
        self.assertEqual([r["at"] for r in b], [i*100_000 for i in range(128)])

    def test_config_changes_only_policy(self):
        self.assertEqual(configuration("fcfs").replace("send_policy = fcfs", "send_policy = fill_open"),
                         configuration("fill_open"))
        for entry in ("mocs_selected = 256", "max_pending_write_dlus = 8", "max_open_dlus_per_channel = 2",
                      "max_timeout_retries = 0", "max_pending_retries = 0", "write_mode = fragment64"):
            self.assertIn(entry, configuration("fcfs"))

    def test_invalid_choice(self):
        with self.assertRaises(ValueError): records("typo")
        with self.assertRaises(ValueError): configuration("typo")

    def test_exclusive_trace_and_config_scope(self):
        with temporary(Path.cwd()) as root:
            trace = root / "trace.txt"
            write_workload(trace, "batched")
            self.assertEqual(len(trace.read_text().splitlines()), 129)
            with self.assertRaises(FileExistsError): write_workload(trace, "sparse")
            config = root / "config.ini"
            config.write_text('[host]\nsend_policy = "fcfs"\n[output]\nstatistics_json="a"\n[other]\nsend_policy="keep"\n', encoding="utf-8")
            self.assertEqual(normalized_effective(config), {"other.send_policy": '"keep"'})


def check(value: bool, why: str) -> None:
    if not value:
        raise AssertionError(why)


def integration(cli: Path, parent: Path) -> None:
    parent.mkdir(parents=True, exist_ok=True)
    parent = parent.resolve()
    with temporary(parent) as root:
        check(root.parent == parent, "temporary directory must stay inside test output")
        runner = ROOT / "experiments/arrival_policy/run.py"
        def scan(target: Path):
            result = subprocess.run([sys.executable, str(runner), "--cli", str(cli), "--output", str(target)],
                                    cwd=ROOT, text=True, capture_output=True, timeout=120)
            check(result.returncode == 0, result.stdout + result.stderr)
            check(json.loads((target / "manifest.json").read_text())["complete"], "comparison checks completed")
            return json.loads((target / "summary.json").read_text())
        first, second = root / "first", root / "repeat"
        rows, repeat = scan(first), scan(second)
        def stable(xs): return [{k:v for k,v in r.items() if k != "process_wall_seconds"} for r in xs]
        check(stable(rows) == stable(repeat), "simulation observations deterministic")
        pair = {(r["shape"], r["policy"]): r for r in rows}
        fc, fill = pair["batched", "fcfs"], pair["batched", "fill_open"]
        check(fill["A_ready_ps"] < fc["A_ready_ps"] and fill["A_programmed_ps"] < fc["A_programmed_ps"],
              "earlier A assembly and successful program")
        check(fc["logical_completion_ps"] == fill["logical_completion_ps"],
              "earlier A does not imply shorter workload completion in this example")
        check(all(r["successful_dlus"] == 2 and r["commands"] == 128 and r["bank_busy_ps"] == 3_000_000 and
                  r["mocs_peak_tick_boundary"] <= 128 and r["pending_peak"] <= 2 for r in rows),
              "same successful work and no capacity pressure")
        for policy in POLICIES:
            data = json.loads((first / "batched" / policy / "statistics.json").read_text())
            a = next(d for d in data["dlu_contexts"] if d["local_dlu"] == 0)
            b = next(d for d in data["dlu_contexts"] if d["local_dlu"] == 8)
            check(a["opened_at"] < 2_000_000 < b["opened_at"] and
                  max(p["received_fragments"] for p in a["progress"] if p["at"] < 2_000_000) == 32,
                  "A open and half filled, B unopened at the competing release")
        overlap = subprocess.run([sys.executable, str(runner), "--cli", str(cli), "--output", str(first)],
                                 capture_output=True, timeout=30)
        check(overlap.returncode != 0, "refuse overwrite")
        bad_rows = copy.deepcopy(rows)
        bad_rows[0]["trace_sha256"] = "changed"
        try: verify_comparisons(bad_rows, first)
        except ValueError: pass
        else: raise AssertionError("trace mismatch accepted")
        config = first / "batched/fill_open/effective.ini"
        original = config.read_text()
        config.write_text(original.replace('mocs_selected = 256', 'mocs_selected = 512'))
        try: verify_comparisons(rows, first)
        except ValueError: pass
        else: raise AssertionError("unequal resource budget accepted")
        config.write_text(original)
        data = json.loads((first / "batched/fcfs/statistics.json").read_text())
        variants = []
        bad = copy.deepcopy(data); bad["logical_work"][0]["local_byte"] += 64; variants.append(bad)
        bad = copy.deepcopy(data); bad["globals"][0]["scheduled_release"] += 1; variants.append(bad)
        bad = copy.deepcopy(data); bad["attempts"].append(bad["attempts"][0]); variants.append(bad)
        bad = copy.deepcopy(data); bad["dlu_contexts"][0]["members"].pop(); variants.append(bad)
        bad = copy.deepcopy(data); bad["pending_channels"][0]["area_context_ticks"]["total"] += 1; variants.append(bad)
        bad = copy.deepcopy(data); bad["bank_operations"][1]["issued_at"] = 0; variants.append(bad)
        bad = copy.deepcopy(data); bad["stop_reason"] = "LimitReached"; variants.append(bad)
        for i, bad in enumerate(variants):
            try: analyze(bad, "batched", first / "batched/fcfs")
            except ValueError: pass
            else: raise AssertionError(f"corrupt observation {i} accepted")
        print("PASS arrival-policy integration: fair pairs, policy choice, sparse control, media lifetimes, determinism and guards")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.cli:
        if not args.output: parser.error("--cli requires --output")
        integration(args.cli.resolve(strict=True), args.output)
    else:
        unittest.main(argv=[sys.argv[0]])
