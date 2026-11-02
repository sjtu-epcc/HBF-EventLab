"""CPU-only model identity fixtures: no CMake, CUDA, model, or Git mutations."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import integrated_build_identity as identity


class BuildIdentityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='real-gpu-identity-')
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name).resolve()
        self.backend = self.work / 'monorepo'
        self.root = self.backend / 'integrations/real_gpu'
        self.build = self.root / 'build-model'
        self.entry = self.root / 'adapters/llama_cpp/integrated'
        self.framework = self.root / 'third_party/llama.cpp-integrated'
        self.make_backend(self.backend)
        for name in identity.FRONTEND_DIRECTORIES:
            self.put(self.root / name / 'fixture.txt', 'frontend ' + name)
        for name in ('CMakeLists.txt', 'build.ps1', 'requirements.txt', 'requirements-dev.txt'):
            self.put(self.root / name, 'frontend root ' + name)
        self.put(self.entry / 'CMakeLists.txt', 'project(realgpu_llama)')
        modified = {}
        for name in ('ggml/src/ggml-cuda/norm.cu', 'ggml/src/ggml-cuda/ggml-cuda.cu',
                     'src/llama-model.cpp', 'src/models/qwen3.cpp', 'src/llama-kv-cache.cpp',
                     'ggml/src/ggml-alloc.c', 'src/models/llama.cpp'):
            path = self.framework / name
            self.put(path, 'fixed framework overlay ' + name)
            modified[name] = identity.sha(path)
        self.put(self.framework / 'hbflab-overlay.json', json.dumps({
            'upstream': identity.FRAMEWORK_COMMIT, 'modified_sha256': modified}))
        self.model = identity.executable(self.build / 'bin', 'realgpu_llama')
        self.put(self.model, 'fixture executable, never executed')
        self.put(self.build / 'bin/ggml-cuda.dll', 'fixture runtime library')
        self.put(self.build / 'bin/helpers/runtime.bin', 'nested helper')
        self.put(self.build / 'weight_kernel.ptx', 'fixture PTX')
        self.cache = {
            'CMAKE_HOME_DIRECTORY': self.entry,
            'CMAKE_PROJECT_NAME': 'realgpu_llama',
            'CMAKE_CACHEFILE_DIR': self.build,
            'HBFLAB_EVENTLAB_SOURCE': self.backend,
            'HBFEventLab_SOURCE_DIR': self.backend,
            'HBFEventLab_BINARY_DIR': self.build / 'eventlab',
            'realgpu_llama_SOURCE_DIR': self.entry,
            'llama.cpp_SOURCE_DIR': self.framework,
            'LLAMA_BUILD_COMMIT': identity.FRAMEWORK_COMMIT,
        }
        self.write_cache()
        root_patch = patch.object(identity, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        head_patch = patch.object(identity, '_git_head', return_value='a' * 40)
        self.git_head = head_patch.start()
        self.addCleanup(head_patch.stop)

    @staticmethod
    def put(path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value, encoding='utf-8')

    def make_backend(self, root):
        self.put(root / 'CMakeLists.txt', 'project(HBFEventLab)')
        self.put(root / 'include/hbf_eventlab/runtime/device_driver.hpp', 'backend API')
        for name in identity.BACKEND_DIRECTORIES:
            self.put(root / name / 'fixture.txt', 'backend ' + name)

    def write_cache(self):
        self.put(self.build / 'CMakeCache.txt', '// fixture cache\n' + ''.join(
            str(key) + ':INTERNAL=' + str(value) + '\n' for key, value in self.cache.items()))

    def assert_changed(self, path, message):
        original = path.read_bytes()
        try:
            path.write_bytes(original + b' changed')
            with self.assertRaisesRegex(RuntimeError, message):
                identity.verify_build(self.build)
        finally:
            path.write_bytes(original)

    def test_seal_verify_success_and_actual_hash_identity(self):
        sealed = identity.seal(self.build)
        self.assertEqual(identity.verify_build(self.build), sealed)
        backend = sealed['backend']
        self.assertEqual(backend['path'], str(self.backend))
        self.assertEqual(backend['baseline_commit'], identity.COMMIT)
        self.assertEqual(identity.COMMIT, '8ccb1f353630f893f761b017d621812b7b9d5bf2')
        self.assertEqual(backend['commit'], 'sha256:' + backend['source_sha256'])
        self.assertNotEqual(backend['commit'], backend['baseline_commit'])
        self.assertEqual(backend['git_head_at_seal'], 'a' * 40)
        self.assertEqual(sealed['provenance']['monorepo_git_head_at_seal'], 'a' * 40)
        self.assertEqual(sealed['framework']['commit'], identity.FRAMEWORK_COMMIT)
        self.assertEqual(len(sealed['framework']['modified_sha256']), 7)
        for expected in (backend['commit'], backend['source_sha256']):
            self.assertEqual(identity.verify_build(self.build, expected), sealed)

    def test_backend_content_changes_in_every_allowlisted_directory(self):
        identity.seal(self.build)
        for name in identity.BACKEND_DIRECTORIES:
            with self.subTest(directory=name):
                self.assert_changed(self.backend / name / 'fixture.txt', 'backend source inventory changed')
        self.assert_changed(self.backend / 'CMakeLists.txt', 'backend source inventory changed')

    def test_backend_inventory_addition_and_deletion(self):
        identity.seal(self.build)
        added = self.backend / 'src/new.cpp'
        self.put(added, 'new source')
        with self.assertRaisesRegex(RuntimeError, 'backend source inventory changed'):
            identity.verify_build(self.build)
        added.unlink()
        existing = self.backend / 'tests/fixture.txt'
        original = existing.read_bytes()
        existing.unlink()
        with self.assertRaisesRegex(RuntimeError, 'backend source inventory changed'):
            identity.verify_build(self.build)
        existing.write_bytes(original)
        identity.verify_build(self.build)

    def test_frontend_root_build_controls_and_directory_changes(self):
        identity.seal(self.build)
        for name in ('CMakeLists.txt', 'build.ps1', 'requirements.txt', 'requirements-dev.txt'):
            with self.subTest(root_input=name):
                self.assert_changed(self.root / name, 'frontend source inventory changed')
        for name in identity.FRONTEND_DIRECTORIES:
            with self.subTest(directory=name):
                self.assert_changed(self.root / name / 'fixture.txt', 'frontend source inventory changed')

    def test_frontend_source_addition_and_deletion(self):
        identity.seal(self.build)
        added = self.root / 'src/new.cpp'
        self.put(added, 'new frontend source')
        with self.assertRaisesRegex(RuntimeError, 'frontend source inventory changed'):
            identity.verify_build(self.build)
        added.unlink()
        (self.root / 'include/fixture.txt').unlink()
        with self.assertRaisesRegex(RuntimeError, 'frontend source inventory changed'):
            identity.verify_build(self.build)

    def test_artifact_content_changes_including_nested_helpers(self):
        identity.seal(self.build)
        for path in (self.model, self.build / 'bin/ggml-cuda.dll',
                     self.build / 'bin/helpers/runtime.bin', self.build / 'weight_kernel.ptx'):
            with self.subTest(artifact=path.name):
                self.assert_changed(path, 'changed file')
        # Change only a comment: semantics are identical, but cache bytes are sealed.
        self.assert_changed(self.build / 'CMakeCache.txt', 'changed file')

    def test_same_size_artifact_change_is_hashed(self):
        identity.seal(self.build)
        original = self.model.read_bytes()
        self.model.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        with self.assertRaisesRegex(RuntimeError, 'changed file'):
            identity.verify_build(self.build)

    def test_missing_artifacts_fail_closed(self):
        identity.seal(self.build)
        for path in (self.model, self.build / 'bin/ggml-cuda.dll',
                     self.build / 'bin/helpers/runtime.bin', self.build / 'weight_kernel.ptx',
                     self.build / 'CMakeCache.txt'):
            with self.subTest(artifact=path.name):
                original = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(RuntimeError, 'missing'):
                    identity.verify_build(self.build)
                path.write_bytes(original)

    def test_added_artifacts_and_removed_manifest_entries_fail(self):
        identity.seal(self.build)
        added = self.build / 'bin/new-runtime.dll'
        self.put(added, 'new runtime')
        with self.assertRaisesRegex(RuntimeError, 'artifact inventory changed'):
            identity.verify_build(self.build)
        added.unlink()
        path = self.build / 'release-identity.json'
        sealed = identity.read(path)
        sealed['artifacts'] = [row for row in sealed['artifacts'] if row['path'] != str(self.model)]
        identity.write(path, sealed)
        with self.assertRaisesRegex(RuntimeError, 'artifact inventory changed'):
            identity.verify_build(self.build)

    def test_seal_requires_executable_and_cache(self):
        original = self.model.read_bytes()
        self.model.unlink()
        with self.assertRaisesRegex(RuntimeError, 'model executable missing'):
            identity.seal(self.build)
        self.model.write_bytes(original)
        (self.build / 'CMakeCache.txt').unlink()
        with self.assertRaisesRegex(RuntimeError, 'CMake cache missing'):
            identity.seal(self.build)
        self.assertFalse((self.build / 'release-identity.json').exists())

    def test_cache_backend_disagreement_rejected_at_seal_and_verify(self):
        actual = self.cache['HBFEventLab_SOURCE_DIR']
        self.cache['HBFEventLab_SOURCE_DIR'] = self.work / 'wrong-backend'
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'cache backend mismatch'):
            identity.seal(self.build)
        self.cache['HBFEventLab_SOURCE_DIR'] = actual
        self.write_cache()
        identity.seal(self.build)
        self.cache['HBFLAB_EVENTLAB_SOURCE'] = self.work / 'wrong-backend'
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'cache backend mismatch'):
            identity.verify_build(self.build)

    def test_actual_backend_override_is_hashed_instead_of_vendor_or_default(self):
        alternate = self.work / 'alternate-backend'
        self.make_backend(alternate)
        self.put(alternate / 'src/fixture.txt', 'actually configured alternate backend')
        self.cache['HBFLAB_EVENTLAB_SOURCE'] = alternate
        self.cache['HBFEventLab_SOURCE_DIR'] = alternate
        self.write_cache()
        sealed = identity.seal(self.build)
        self.assertEqual(sealed['backend']['path'], str(alternate))
        self.put(self.backend / 'src/fixture.txt', 'unused default backend changed')
        self.assertEqual(identity.verify_build(self.build), sealed)
        self.assert_changed(alternate / 'src/fixture.txt', 'backend source inventory changed')

    def test_backend_location_change_rejected_even_with_identical_sources(self):
        identity.seal(self.build)
        alternate = self.work / 'identical-backend-at-new-location'
        self.make_backend(alternate)
        self.cache['HBFLAB_EVENTLAB_SOURCE'] = alternate
        self.cache['HBFEventLab_SOURCE_DIR'] = alternate
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'backend location changed'):
            identity.verify_build(self.build)

    def test_missing_or_relative_backend_cache_path_is_not_guessed(self):
        value = self.cache.pop('HBFLAB_EVENTLAB_SOURCE')
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'cache entry missing: HBFLAB_EVENTLAB_SOURCE'):
            identity.seal(self.build)
        self.cache['HBFLAB_EVENTLAB_SOURCE'] = '../..'
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'cache path must be absolute'):
            identity.seal(self.build)
        self.cache['HBFLAB_EVENTLAB_SOURCE'] = value

    def test_seal_rejects_non_model_source_entry_despite_stale_executable(self):
        self.cache['CMAKE_HOME_DIRECTORY'] = self.root
        self.cache['CMAKE_PROJECT_NAME'] = 'HBFLab'
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'not the integrated model entry'):
            identity.seal(self.build)
        self.assertFalse((self.build / 'release-identity.json').exists())

    def test_cache_build_location_and_project_are_checked(self):
        self.cache['CMAKE_CACHEFILE_DIR'] = self.work / 'another-build'
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'cache build location changed'):
            identity.seal(self.build)
        self.cache['CMAKE_CACHEFILE_DIR'] = self.build
        self.cache['CMAKE_PROJECT_NAME'] = 'HBFLab'
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'cache project is not the integrated model'):
            identity.seal(self.build)

    def test_auxiliary_cache_locations_are_checked(self):
        for key in ('HBFEventLab_BINARY_DIR', 'llama.cpp_SOURCE_DIR', 'realgpu_llama_SOURCE_DIR'):
            with self.subTest(entry=key):
                original = self.cache[key]
                self.cache[key] = self.work / 'wrong-location'
                self.write_cache()
                with self.assertRaisesRegex(RuntimeError, 'cache location mismatch'):
                    identity.seal(self.build)
                self.cache[key] = original

    def test_build_and_frontend_locations_are_bound_to_seal(self):
        identity.seal(self.build)
        moved = self.work / 'moved-build'
        shutil.copytree(self.build, moved)
        with self.assertRaisesRegex(RuntimeError, 'build location changed'):
            identity.verify_build(moved)
        path = self.build / 'release-identity.json'
        sealed = identity.read(path)
        sealed['frontend']['path'] = str(self.work / 'other-frontend')
        identity.write(path, sealed)
        with self.assertRaisesRegex(RuntimeError, 'frontend location changed'):
            identity.verify_build(self.build)

    def test_expected_mismatch_including_baseline_and_git_head(self):
        identity.seal(self.build)
        for expected in ('sha256:' + '0' * 64, '0' * 64, identity.COMMIT, 'a' * 40, ''):
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(RuntimeError, 'expected backend mismatch'):
                    identity.verify_build(self.build, expected)

    def test_git_head_is_provenance_not_live_identity(self):
        sealed = identity.seal(self.build)
        self.git_head.reset_mock()
        self.git_head.return_value = 'b' * 40
        self.put(self.backend / 'docs/new.md', 'unrelated documentation change')
        self.put(self.backend / 'README.md', 'another documentation-only commit')
        self.assertEqual(identity.verify_build(self.build), sealed)
        self.git_head.assert_not_called()
        self.assertEqual(sealed['backend']['git_head_at_seal'], 'a' * 40)

    def test_inventory_ignores_generated_outputs_and_backend_integrations(self):
        sealed = identity.seal(self.build)
        for relative in ('build-new/result.bin', 'integrations/other/src/noise.cpp',
                         'src/__pycache__/noise.pyc', 'experiments/build-cuda/result.obj',
                         'tests/output/result.json'):
            self.put(self.backend / relative, 'not a backend source input')
        for relative in ('tools/__pycache__/helper.pyc', 'third_party/pkg/build/result.bin',
                         'src/temp.obj', 'output/log.json', 'build-another/cache.txt'):
            self.put(self.root / relative, 'not a frontend source input')
        self.assertEqual(identity.verify_build(self.build), sealed)
        backend_paths = [row['path'] for row in sealed['backend']['sources']]
        self.assertFalse(any('integrations' in Path(path).relative_to(self.backend).parts
                             for path in backend_paths))

    def test_all_framework_overlay_files_are_checked_before_seal(self):
        overlay = identity.read(self.framework / 'hbflab-overlay.json')
        for name in overlay['modified_sha256']:
            with self.subTest(overlay=name):
                path = self.framework / name
                original = path.read_bytes()
                path.write_bytes(original + b'wrong')
                with self.assertRaisesRegex(RuntimeError, 'framework overlay changed'):
                    identity.seal(self.build)
                path.write_bytes(original)
        self.cache['LLAMA_BUILD_COMMIT'] = '0' * 40
        self.write_cache()
        with self.assertRaisesRegex(RuntimeError, 'framework commit mismatch'):
            identity.seal(self.build)

    def test_backend_identity_metadata_cannot_replace_file_hashes(self):
        sealed = identity.seal(self.build)
        path = self.build / 'release-identity.json'
        sealed['backend']['commit'] = identity.COMMIT
        identity.write(path, sealed)
        with self.assertRaisesRegex(RuntimeError, 'backend source identity mismatch'):
            identity.verify_build(self.build)

    def test_sealed_build_cannot_be_overwritten_and_old_schema_is_rejected(self):
        identity.seal(self.build)
        with self.assertRaisesRegex(RuntimeError, 'sealed build cannot be overwritten'):
            identity.seal(self.build)
        identity.write(self.build / 'release-identity.json', {'status': 'sealed', 'backend': {'commit': identity.COMMIT}})
        with self.assertRaisesRegex(RuntimeError, 'unsupported build identity'):
            identity.verify_build(self.build)

    def test_runner_arguments_and_records_helper_remain_compatible(self):
        parser = argparse.ArgumentParser()
        identity.add_build_arguments(parser)
        args = parser.parse_args([])
        self.assertIsNone(args.expected_backend)
        self.assertEqual(args.gpu_device, '0')
        self.assertEqual(args.build_dir, identity.DEFAULT_BUILD)
        args = parser.parse_args(['--build-dir', str(self.build), '--expected-backend', 'sha256:abc',
                                  '--cuda-root', str(self.work / 'cuda'), '--gpu-device', '2'])
        self.assertEqual(args.build_dir, self.build)
        self.assertEqual(args.expected_backend, 'sha256:abc')
        self.assertEqual(args.gpu_device, '2')
        rows = identity.records([self.model])
        self.assertEqual(rows, [{'path': str(self.model), 'bytes': self.model.stat().st_size,
                                 'sha256': hashlib.sha256(self.model.read_bytes()).hexdigest()}])
        identity.verify_records(rows)


if __name__ == '__main__':
    unittest.main()
