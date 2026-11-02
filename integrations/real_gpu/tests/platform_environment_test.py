"""Migration settings must work without the original machine's paths."""
import os
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from platform_environment import executable, runtime_environment
from package_source import package, sources


class EnvironmentTests(unittest.TestCase):
    def test_cache_runtime_discovery_and_device(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); build = root / 'build-new'; build.mkdir()
            cuda = root / 'toolkit with spaces'; (cuda / 'bin').mkdir(parents=True)
            (build / 'bin').mkdir()
            (build / 'CMakeCache.txt').write_text('CUDAToolkit_ROOT:PATH=' + str(cuda) + '\n')
            env = runtime_environment(build, root, gpu_device='2')
            self.assertEqual(env['CUDA_PATH'], str(cuda.resolve()))
            self.assertEqual(env['CUDA_VISIBLE_DEVICES'], '2')
            self.assertIn(str(cuda / 'bin'), env['PATH'].split(os.pathsep))
            override = root / 'override'; override.mkdir()
            self.assertEqual(runtime_environment(build, root, override)['CUDA_PATH'], str(override))

    def test_monorepo_build_paths_are_filtered(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            backend = root / 'build-named-repository'
            source = backend / 'integrations' / 'real_gpu'
            source.mkdir(parents=True)
            build = backend / 'build-current'
            (build / 'bin').mkdir(parents=True)
            cuda = backend / 'toolkits' / 'cuda'
            (cuda / 'bin').mkdir(parents=True)
            stale = [backend / 'build-old' / 'bin', source / 'build-old' / 'bin']
            # Match build-* only immediately below a known source root, not any
            # ancestor or unrelated nested toolkit directories.
            retained = [backend / 'tools' / 'bin', source / 'tools' / 'bin',
                        source / 'toolkits' / 'build-toolkit' / 'bin',
                        root / 'other' / 'build-old' / 'bin']
            inherited = os.pathsep.join(map(str, stale + retained))
            with patch.dict(os.environ, {'PATH': inherited, 'LD_LIBRARY_PATH': inherited}, clear=True):
                env = runtime_environment(build, source, cuda)
            paths = env['PATH'].split(os.pathsep)
            self.assertEqual(paths[0], str(build / 'bin'))
            for path in stale:
                self.assertNotIn(str(path), paths)
            for path in retained:
                self.assertIn(str(path), paths)
            if os.name != 'nt':
                for path in stale:
                    self.assertNotIn(str(path), env['LD_LIBRARY_PATH'].split(os.pathsep))

    def test_configured_external_backend_paths_are_filtered(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); source = root / 'frontend'; source.mkdir()
            backend = root / 'external backend'; backend.mkdir()
            build = source / 'build-current'; build.mkdir()
            (build / 'CMakeCache.txt').write_text('HBFLAB_EVENTLAB_SOURCE:PATH=' + str(backend) + '\n')
            stale = backend / 'build-previous' / 'bin'
            keep = backend / 'toolkit' / 'bin'
            with patch.dict(os.environ, {'PATH': os.pathsep.join(map(str, [stale, keep]))}, clear=True):
                paths = runtime_environment(build, source)['PATH'].split(os.pathsep)
            self.assertNotIn(str(stale), paths)
            self.assertIn(str(keep), paths)

    def test_toolkit_under_build_directory_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            backend = Path(folder); source = backend / 'integrations' / 'real_gpu'
            source.mkdir(parents=True)
            build = backend / 'build-current'; build.mkdir()
            cuda = backend / 'build-toolkit' / 'cuda'
            # This nonstandard toolkit directory is inherited, not auto-added.
            inherited_tool = cuda / 'custom-tools'; inherited_tool.mkdir(parents=True)
            stale = cuda.parent / 'stale-dlls'
            with patch.dict(os.environ, {'PATH': os.pathsep.join(map(str, [inherited_tool, stale]))}, clear=True):
                paths = runtime_environment(build, source, cuda)['PATH'].split(os.pathsep)
            self.assertIn(str(inherited_tool), paths)
            self.assertNotIn(str(stale), paths)

    def test_explicit_cupti_sdk_under_backend_build_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            backend = Path(folder); source = backend / 'integrations' / 'real_gpu'
            source.mkdir(parents=True)
            build = backend / 'build-current'; (build / 'bin').mkdir(parents=True)
            sdk = backend / 'build-cupti-sdk' / 'official archive'
            (sdk / 'bin').mkdir(parents=True); (sdk / 'lib').mkdir()
            cuda = backend / 'cuda'; (cuda / 'bin').mkdir(parents=True)
            cache = build / 'CMakeCache.txt'
            cache.write_text('HBFLAB_CUPTI_ROOT:PATH=' + str(sdk) + '\n')
            stale = backend / 'build-old' / 'bin'
            with patch.dict(os.environ, {'PATH': str(stale)}, clear=True):
                env = runtime_environment(build, source, cuda)
            paths = env['PATH'].split(os.pathsep)
            self.assertEqual(paths[0], str(build / 'bin'))
            self.assertIn(str(sdk / 'bin'), paths)
            self.assertIn(str(sdk / 'lib'), paths)
            self.assertLess(paths.index(str(sdk / 'bin')), paths.index(str(cuda / 'bin')))
            self.assertNotIn(str(sdk / 'lib64'), paths)
            self.assertNotIn(str(stale), paths)
            if os.name != 'nt':
                self.assertIn(str(sdk / 'lib'), env['LD_LIBRARY_PATH'].split(os.pathsep))
            cache.write_text('HBFLAB_CUPTI_ROOT:PATH=' + str(sdk / 'missing') + '\n')
            with self.assertRaisesRegex(ValueError, 'CUPTI runtime root does not exist'):
                runtime_environment(build, source, cuda)

    def test_standalone_package_create_is_rejected_without_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = root / 'release-manifest.json'
            manifest.write_text('historical manifest\n')
            archive = root / 'new-output' / 'source.zip'
            with patch('package_source.ROOT', root):
                with self.assertRaisesRegex(RuntimeError, 'git archive --format=zip --output='):
                    package(archive)
            self.assertEqual(manifest.read_text(), 'historical manifest\n')
            self.assertFalse(archive.parent.exists())
            self.assertEqual(list(root.iterdir()), [manifest])

    def test_invalid_toolkit_fails_before_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):
                runtime_environment(folder, folder, Path(folder) / 'missing')

    def test_native_executable_name(self):
        self.assertEqual(executable('bin', 'runner').name,
                         'runner.exe' if os.name == 'nt' else 'runner')

    def test_local_machine_paths_are_not_packaged(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); config = root / 'configs'; config.mkdir()
            (config / 'machine.local.json').write_text('{}')
            (config / 'machine.example.json').write_text('{}')
            self.assertEqual([p.name for p in sources(root)], ['machine.example.json'])


if __name__ == '__main__':
    unittest.main()
