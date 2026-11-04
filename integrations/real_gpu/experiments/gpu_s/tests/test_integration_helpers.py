"""CPU-only integration helpers; no CUDA or model download is performed."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE))
sys.path.insert(0, str(SOURCE.parents[1]/'tools'))
from platform_environment import runtime_environment
from report import criteria_text


class IntegrationHelpers(unittest.TestCase):
    def test_cublas_uses_configured_sdk_not_stale_build(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            frontend = root/'integrations/real_gpu'; frontend.mkdir(parents=True)
            base = root/'build-model'; (base/'bin').mkdir(parents=True)
            cuda = root/'cuda'; (cuda/'bin').mkdir(parents=True)
            sdk = root/'build-sdk/cublas'; (sdk/'lib/x64').mkdir(parents=True); (sdk/'bin').mkdir()
            lib = sdk/'lib/x64/cublas.lib'; lib.write_bytes(b'fixture')
            old = root/'build-unrelated/bin'; old.mkdir(parents=True)
            (base/'CMakeCache.txt').write_text(f'CUDAToolkit_ROOT:PATH={cuda}\nCUDA_cublas_LIBRARY:FILEPATH={lib}\n')
            with patch.dict(os.environ, {'PATH': str(old)}):
                env = runtime_environment(base, frontend)
            paths = env['PATH'].split(os.pathsep)
            self.assertIn(str(sdk/'bin'), paths)
            self.assertNotIn(str(old), paths)
            self.assertEqual(paths[0], str(base/'bin'))
            self.assertEqual(env['GGML_CUDA_DISABLE_GRAPHS'], '1')

    def test_missing_selected_cublas_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); base = root/'build'; base.mkdir()
            (base/'CMakeCache.txt').write_text(f'CUDA_cublas_LIBRARY:FILEPATH={root}/missing.lib\n')
            with self.assertRaisesRegex(ValueError, 'cuBLAS'):
                runtime_environment(base, root)

    def test_old_uniform_threshold_report(self):
        text = criteria_text({'relative_rms_max': 0.125, 'element_error_over_max_reference_abs_or_reference_rms': 0.25})
        self.assertIn('0.125', text); self.assertIn('0.25', text)

    def test_classified_threshold_report(self):
        criteria = {name: {'relative_rms_max': 0.125, 'normalized_max': 0.25}
                    for name in ('hidden_qkv', 'kv_storage', 'logits')}
        text = criteria_text(criteria)
        for name in criteria: self.assertIn(name, text)


if __name__ == '__main__': unittest.main()
