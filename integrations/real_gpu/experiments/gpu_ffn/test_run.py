"""Independent geometry/accounting and corrupt-capture rejection tests."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys

spec = importlib.util.spec_from_file_location('ffn_run', Path(__file__).with_name('run.py'))
ffn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ffn)


class ExperimentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ffn.ROOT/'output').mkdir(exist_ok=True)

    def tensors(self):
        return {f'blk.{layer}.ffn_{kind}.weight': {
            'shape': [3072, 1024] if kind == 'down' else [1024, 3072],
            'dtype': 'F16', 'bytes': 6291456}
            for layer in range(28) for kind in ('gate', 'up', 'down')}

    def test_stable_full_layout_and_f1_counts(self):
        layout = ffn.make_layout(self.tensors())
        self.assertEqual(len(layout), 84)
        self.assertEqual(layout[-1]['global_base']+layout[-1]['valid_bytes'], 504*1024**2)
        f1 = [r for r in layout if r['layer'] == 10]
        self.assertEqual([r['global_base'] for r in f1], [n*1024**2 for n in (180, 186, 192)])
        self.assertEqual(ffn.expected_counts(f1, 8), {
            'registered_valid_bytes': 18874368, 'parent_reads': 24,
            'cumulative_read_bytes': 150994944, 'read_globals': 36864,
            'read_fragments': 2359296, 'write_fragments': 294912})

    def test_actual_tensor_contract(self):
        tensors = self.tensors()
        tensors['blk.27.ffn_up.weight']['dtype'] = 'Q8_0'
        with self.assertRaisesRegex(ValueError, 'Unsupported FFN tensor'):
            ffn.make_layout(tensors)

    def test_f2_selection_and_counts(self):
        layout = ffn.make_layout(self.tensors())
        regions = ffn.select_regions(layout, 'F2')
        self.assertEqual(sorted({r['layer'] for r in regions}), [0, 10, 20, 27])
        self.assertEqual(len(regions), 12)
        self.assertEqual(ffn.expected_counts(regions, 8), {
            'registered_valid_bytes': 75497472, 'parent_reads': 96,
            'cumulative_read_bytes': 603979776, 'read_globals': 147456,
            'read_fragments': 9437184, 'write_fragments': 1179648})
        self.assertEqual([r for r in regions if r['layer'] == 10], ffn.select_regions(layout, 'F1'))

    def test_unknown_stage_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Unsupported'):
            ffn.select_regions(ffn.make_layout(self.tensors()), 'F4')

    def test_f3_full_coverage_and_capacity(self):
        layout = ffn.make_layout(self.tensors())
        regions = ffn.select_regions(layout, 'F3')
        self.assertEqual(regions, layout)
        self.assertEqual(len({r['tensor'] for r in regions}), 84)
        end = 0
        for region in regions:
            self.assertEqual(region['global_base'], end)
            end += region['valid_bytes']
        self.assertEqual(512*1024**2-end, 8*1024**2)
        self.assertEqual(ffn.expected_counts(regions, 8), {
            'registered_valid_bytes': 528482304, 'parent_reads': 672,
            'cumulative_read_bytes': 4227858432, 'read_globals': 1032192,
            'read_fragments': 66060288, 'write_fragments': 8257536})
        self.assertEqual(ffn.expected_counts(regions, 1)['parent_reads'], 84)
        self.assertEqual([r for r in regions if r['layer'] in (0, 10, 20, 27)],
                         ffn.select_regions(layout, 'F2'))

    def test_cli_failure_preserves_existing_output(self):
        with tempfile.TemporaryDirectory(dir=ffn.ROOT/'output') as name:
            output = Path(name)
            marker = output/'keep.txt'
            marker.write_text('existing experiment')
            process = subprocess.run([sys.executable, str(Path(__file__).with_name('run.py')),
                                      '--model', str(output/'missing.gguf'), '--output', str(output)],
                                     capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(marker.read_text(), 'existing experiment')
            self.assertFalse((output/'runner-failure.json').exists())

    def test_missing_tensor(self):
        tensors = self.tensors()
        del tensors['blk.10.ffn_gate.weight']
        with self.assertRaises(KeyError):
            ffn.make_layout(tensors)

    def test_union_counts_overlap_once(self):
        self.assertEqual(ffn.union_ns([(12, 30), (0, 20), (40, 41)]), 31)
        self.assertEqual(ffn.union_ns([]), 0)

    def test_capture_rejects_unowned_activity(self):
        with tempfile.TemporaryDirectory(dir=ffn.ROOT/'output') as name:
            p = Path(name)
            activity = {'activity_id': 1, 'step': 0, 'start_ns': 10, 'end_ns': 20}
            (p/'kernels.jsonl').write_text(json.dumps(activity)+'\n')
            (p/'transfers.jsonl').write_text('')
            window = {'segment_id': 1, 'kind': 'node', 'node_id': 1, 'step': 0,
                      'first_activity_id': 1, 'activity_count': 1}
            (p/'segments.jsonl').write_text(json.dumps(window)+'\n')
            table = ffn.capture_table(p, {})
            self.assertEqual(table['segments'][0]['duration_ps'], 10000)
            window['activity_count'] = 0
            (p/'segments.jsonl').write_text(json.dumps(window)+'\n')
            with self.assertRaisesRegex(ValueError, 'omitted'):
                ffn.capture_table(p, {})

    def test_capture_rejects_duplicate_ownership(self):
        with tempfile.TemporaryDirectory(dir=ffn.ROOT/'output') as name:
            p = Path(name)
            (p/'kernels.jsonl').write_text(json.dumps({'activity_id': 1, 'step': 0, 'start_ns': 10, 'end_ns': 20})+'\n')
            (p/'transfers.jsonl').write_text('')
            window = {'kind': 'node', 'node_id': 1, 'step': 0, 'first_activity_id': 1, 'activity_count': 1}
            (p/'segments.jsonl').write_text('\n'.join(json.dumps(dict(window, segment_id=i)) for i in (1, 2)))
            with self.assertRaisesRegex(ValueError, 'Overlapping'):
                ffn.capture_table(p, {})


if __name__ == '__main__':
    unittest.main()
