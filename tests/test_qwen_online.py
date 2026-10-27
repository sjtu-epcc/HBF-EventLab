"""Audit rejection tests; no CUDA, model weights, NumPy or PyTorch required."""
from pathlib import Path
import sys
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments.qwen_online.verify import generation, timeline, union_bytes

class AuditTests(unittest.TestCase):
    def fixture(self):
        result = dict(success=True, tail_complete=True, gates=2, initialization_end_tick=100,
                      compute_interval_ps=7, tail_requested_ps=9, tail_start=127, tail_end=136,
                      inference_end_tick=136, compute_total_ps=16, simulated_inference_ticks=36,
                      wall_seconds=10, inference_wall_seconds=5, backend_wall_seconds=3,
                      executor_and_dump_wall_seconds=2)
        gates = [dict(sequence="1", compute_ps="0", arrival_tick="100", completed_tick="120", success="1"),
                 dict(sequence="2", compute_ps="7", arrival_tick="127", completed_tick="127", success="1")]
        return result, gates
    def test_valid_timeline(self):
        result, gates = self.fixture()
        self.assertEqual(timeline(result, gates)["selected_memory_service_ticks"], 20)
    def test_tail_omission(self):
        result, gates = self.fixture();result["tail_end"] = 127
        with self.assertRaises(ValueError):timeline(result, gates)
    def test_wall_clock_injection(self):
        result, gates = self.fixture();gates[1]["arrival_tick"] = "130"
        with self.assertRaises(ValueError):timeline(result, gates)
    def test_failed_callback(self):
        result, gates = self.fixture();gates[0]["success"] = "0"
        with self.assertRaises(ValueError):timeline(result, gates)
    def test_missing_gate(self):
        result, gates = self.fixture()
        with self.assertRaises(ValueError):timeline(result, gates[:1])
    def test_nonfinite_wall(self):
        result, gates = self.fixture();result["wall_seconds"] = float("nan")
        with self.assertRaises(ValueError):timeline(result, gates)
    def test_generation(self):
        reference = dict(tokens=[3], positions=2, stop="length")
        result = dict(reference, success=True)
        generation(reference, result)
        for field, value in (("tokens", [4]), ("positions", 3), ("stop", "EOS"), ("success", False)):
            bad = dict(result);bad[field] = value
            with self.assertRaises(ValueError):generation(reference, bad)
    def test_coverage_union(self):
        self.assertEqual(union_bytes([(0, 64), (0, 64), (32, 128), (128, 256)]), 256)
        self.assertEqual(union_bytes([(0, 64), (128, 192)]), 128)
        with self.assertRaises(ValueError):union_bytes([(2, 1)])

if __name__ == "__main__":unittest.main()
