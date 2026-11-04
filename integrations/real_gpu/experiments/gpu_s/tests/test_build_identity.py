"""Small mutation tests for the separate S build seal (no real compiler/GPU)."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE))
import build_identity as identity
from platform_environment import executable


class ExperimentIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root/'source'; self.source.mkdir()
        (self.source/'native_s8.cpp').write_text('fixture source\n')
        self.build = self.root/'build'; (self.build/'bin').mkdir(parents=True)
        (self.build/'eventlab').mkdir()
        self.backend = self.root/'backend'; self.backend.mkdir()
        self.base = self.root/'base'; self.base.mkdir()
        (self.base/'release-identity.json').write_text('{}')
        self.sdk = self.root/'sdk'; (self.sdk/'include').mkdir(parents=True); (self.sdk/'lib').mkdir()
        (self.sdk/'include/cupti.h').write_text('fixture header')
        (self.sdk/'lib/cupti.lib').write_text('fixture lib')
        (self.sdk/'lib/cudart.lib').write_text('fixture cudart')
        for name in ('gpu_s_native','gpu_s_s8','gpu_s_bridge'):
            executable(self.build/'bin',name).write_bytes(b'fixture binary')
        self.cache = {'CMAKE_HOME_DIRECTORY': str(self.source), 'CMAKE_CACHEFILE_DIR': str(self.build),
                      'CMAKE_PROJECT_NAME': 'gpu_s_stages', 'HBFLAB_EVENTLAB_SOURCE': str(self.backend),
                      'HBFEventLab_SOURCE_DIR': str(self.backend), 'BASE_BUILD': str(self.base),
                      'S_CUPTI_INCLUDE': str(self.sdk/'include'), 'S_CUPTI_LIBRARY': str(self.sdk/'lib/cupti.lib'),
                      'CUDAToolkit_ROOT': str(self.sdk), 'CUDA_cudart_LIBRARY': str(self.sdk/'lib/cudart.lib')}
        self.base_cache = {'CUDAToolkit_ROOT': str(self.sdk), 'CUDA_cudart_LIBRARY': str(self.sdk/'lib/cudart.lib'),
                           'M2_CUPTI_INCLUDE': str(self.sdk/'include'), 'M2_CUPTI_LIBRARY': str(self.sdk/'lib/cupti.lib')}
        for name in ('llama','ggml','ggml-base'):
            path = self.base/(name+'.lib'); path.write_bytes(b'dependency')
            self.cache['S_LIBRARY_'+name] = str(path)
        (self.build/'CMakeCache.txt').write_text('fixture cache')
        self.patches = [patch.object(identity,'SOURCE',self.source),
                        patch.object(identity,'_cache',side_effect=lambda p: self.cache if p==self.build else self.base_cache),
                        patch.object(identity,'verify_dependency',return_value={'backend': {'path': str(self.backend), 'source_sha256':'abc'}}),
                        patch.object(identity,'backend_inputs',return_value=[]), patch.object(identity,'_source_hash',return_value='abc')]
        for item in self.patches: item.start(); self.addCleanup(item.stop)

    def test_seal_and_verify_no_overwrite(self):
        expected = identity.seal(self.build)
        self.assertEqual(identity.verify(self.build),expected)
        with self.assertRaisesRegex(RuntimeError,'overwrite'): identity.seal(self.build)

    def test_source_mutation_rejected(self):
        identity.seal(self.build)
        (self.source/'native_s8.cpp').write_text('changed')
        with self.assertRaisesRegex(RuntimeError,'changed'): identity.verify(self.build)

    def test_added_binary_rejected(self):
        identity.seal(self.build)
        (self.build/'bin/extra.dll').write_bytes(b'new')
        with self.assertRaisesRegex(RuntimeError,'changed'): identity.verify(self.build)

    def test_external_import_rejected(self):
        outside = self.root/'other.lib'; outside.write_bytes(b'wrong dependency')
        self.cache['S_LIBRARY_llama'] = str(outside)
        with self.assertRaisesRegex(RuntimeError,'outside'): identity.snapshot(self.build)

    def test_actual_backend_mismatch_rejected(self):
        self.cache['HBFEventLab_SOURCE_DIR'] = str(self.root/'wrong')
        with self.assertRaisesRegex(RuntimeError,'backend source'): identity.snapshot(self.build)

    def test_sdk_mutation_rejected(self):
        identity.seal(self.build)
        (self.sdk/'include/cupti.h').write_text('changed header')
        with self.assertRaisesRegex(RuntimeError,'changed'): identity.verify(self.build)

    def test_missing_sdk_library_rejected(self):
        self.cache['S_CUPTI_LIBRARY'] = str(self.sdk/'missing.lib')
        self.base_cache['M2_CUPTI_LIBRARY'] = self.cache['S_CUPTI_LIBRARY']
        with self.assertRaisesRegex(RuntimeError,'SDK input missing'): identity.snapshot(self.build)

    def test_base_experiment_sdk_mismatch_rejected(self):
        self.base_cache['M2_CUPTI_LIBRARY'] = str(self.root/'other-cupti.lib')
        with self.assertRaisesRegex(RuntimeError,'SDK mismatch'): identity.snapshot(self.build)


if __name__ == '__main__': unittest.main()
