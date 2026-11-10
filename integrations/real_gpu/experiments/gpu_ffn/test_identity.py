"""FFN build seals must identify the current backend and shared CUDA source."""
import sys
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ffn_identity as ffn
sys.path.insert(0, str(Path(__file__).resolve().parent.parent/'gpu_s/tests'))
from test_build_identity import ExperimentIdentityTests
import build_identity as identity
from platform_environment import executable

fixture_setup = ExperimentIdentityTests.setUp

class FfnIdentityTests(unittest.TestCase):
    def setUp(self):
        fixture_setup(self)
        self.cache['CMAKE_PROJECT_NAME'] = 'gpu_ffn'
        (self.source/'cupti_compat.hpp').write_text('fixture compatibility')
        for name in ('gpu_ffn', 'ffn_audit_tests'):
            executable(self.build/'bin', name).write_bytes(b'ffn binary')
        mocked = patch.object(ffn, 'OPTIONS', dict(source=self.source, project='gpu_ffn', targets=('gpu_ffn','ffn_audit_tests')))
        mocked.start(); self.addCleanup(mocked.stop)

    def test_roundtrip(self):
        self.assertEqual(ffn.seal(self.build), ffn.verify(self.build))

    def test_missing_ffn_executable(self):
        executable(self.build/'bin', 'gpu_ffn').unlink()
        with self.assertRaisesRegex(RuntimeError, 'executable missing'):
            ffn.seal(self.build)

    def test_backend_mismatch(self):
        self.cache['HBFEventLab_SOURCE_DIR'] = str(self.root/'vendor')
        with self.assertRaisesRegex(RuntimeError, 'backend source'):
            ffn.seal(self.build)

    def test_shared_source_changed(self):
        ffn.seal(self.build)
        (self.source/'native_s8.cpp').write_text('modified shared executor')
        with self.assertRaisesRegex(RuntimeError, 'changed'):
            ffn.verify(self.build)

    def test_dependency_identity_changed(self):
        ffn.seal(self.build)
        (self.base/'release-identity.json').write_text('{"changed":true}')
        with self.assertRaisesRegex(RuntimeError, 'changed'):
            ffn.verify(self.build)

# The fixture class is reused for setup only, not collected twice.
del ExperimentIdentityTests
if __name__ == '__main__': unittest.main()
