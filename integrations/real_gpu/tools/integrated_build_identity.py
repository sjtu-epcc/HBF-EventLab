"""Seal model builds against their actual CMake source inputs, not a Git label.

COMMIT is the phase-1 backend *baseline*, retained for import compatibility.  It
is not evidence of the current backend contents.  backend['commit'] remains a
string for legacy runners, but now contains the actual ``sha256:<inventory>``
source identity; baseline_commit and Git HEAD provenance are separate fields.

The default expected=None verifies the sealed source identity.  An explicit
expected must be that identity or its raw SHA-256, never a self-reported baseline
or Git HEAD.  Git HEAD is recorded only at seal time: a new commit changing only
files outside the inventories (e.g. root docs) does not invalidate a build.  Any
change inside an inventory does, whether committed or not.  This is a local
integrity seal, not a signature or proof against someone replacing the seal.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from setup_llama_baseline import ROOT, sha, COMMIT as FRAMEWORK_COMMIT
from platform_environment import executable

COMMIT = '8ccb1f353630f893f761b017d621812b7b9d5bf2'
BASELINE_COMMIT = COMMIT
DEFAULT_BUILD = ROOT / 'build-model'
DEFAULT_BACKEND = (ROOT / '../..').resolve()
SCHEMA_VERSION = 2
FRONTEND_DIRECTORIES = ('include', 'src', 'cmake', 'adapters', 'tools', 'third_party')
BACKEND_DIRECTORIES = ('src', 'include', 'cmake', 'configs', 'tests', 'examples', 'experiments')
IGNORED_DIRECTORIES = frozenset({
    '.git', '.hg', '.svn', '__pycache__', '.pytest_cache', '.mypy_cache',
    '.ruff_cache', '.venv', 'venv', 'node_modules', 'cmakefiles', '_deps',
    'build', '_build', 'output', 'outputs', 'dist', 'artifacts',
})
GENERATED_SUFFIXES = frozenset({
    '.pyc', '.pyo', '.obj', '.o', '.a', '.lib', '.dll', '.exe', '.so',
    '.dylib', '.pdb', '.ilk', '.exp', '.cubin', '.fatbin',
})
GENERATED_FILES = frozenset({'cmakecache.txt', 'cmake_install.cmake', 'build.ninja',
                             '.ninja_deps', '.ninja_log'})


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def read(path):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise RuntimeError('cannot read identity JSON: ' + str(path)) from error


def write(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def records(paths):
    """Legacy runner helper: absolute paths, byte counts and content hashes."""
    return [{'path': str(p.resolve()), 'bytes': p.stat().st_size, 'sha256': sha(p)}
            for p in sorted(set(map(Path, paths))) if p.is_file()]


def verify_records(items):
    for item in items:
        path = Path(item['path'])
        require(path.is_file(), 'missing file: ' + str(path))
        require(path.stat().st_size == item['bytes'] and sha(path) == item['sha256'],
                'changed file: ' + str(path))


def _ignored_directory(name):
    name = name.lower()
    return (name in IGNORED_DIRECTORIES or name.startswith(('build-', 'build_', 'cmake-build-')))


def _source_records(root, directories, root_files):
    """Walk only allowlisted inputs; never traverse backend integration/build trees."""
    root = Path(root).resolve()
    paths = list(root_files)
    for name in directories:
        folder = root / name
        if not folder.exists():
            continue
        require(folder.is_dir() and not folder.is_symlink(), 'invalid source directory: ' + str(folder))
        for current, dirs, files in os.walk(folder, followlinks=False):
            dirs[:] = sorted(d for d in dirs if not _ignored_directory(d))
            for directory in dirs:
                require(not (Path(current) / directory).is_symlink(),
                        'source directory symlink is not supported: ' + str(Path(current) / directory))
            for filename in sorted(files):
                path = Path(current) / filename
                if path.suffix.lower() in GENERATED_SUFFIXES or filename.lower() in GENERATED_FILES:
                    continue
                require(path.resolve().is_relative_to(root), 'source path escapes root: ' + str(path))
                require(path.is_file(), 'missing source file: ' + str(path))
                paths.append(path)
    return records(paths)


def inputs():
    """Frontend inputs include root build controls, not just source directories."""
    root = ROOT.resolve()
    required = [root / name for name in ('CMakeLists.txt', 'build.ps1', 'requirements.txt')]
    for path in required:
        require(path.is_file(), 'frontend input missing: ' + str(path))
    return _source_records(root, FRONTEND_DIRECTORIES,
                           required + list(root.glob('requirements*.txt')))


def backend_inputs(root):
    root = Path(root).resolve()
    for name in ('CMakeLists.txt', 'include/hbf_eventlab/runtime/device_driver.hpp'):
        require((root / name).is_file(), 'backend input missing: ' + str(root / name))
    require((root / 'src').is_dir(), 'backend src directory missing: ' + str(root))
    return _source_records(root, BACKEND_DIRECTORIES, [root / 'CMakeLists.txt'])


def _source_hash(root, items):
    # The digest is portable; locations are checked independently by the seal.
    root = Path(root).resolve()
    relative = [{'path': Path(i['path']).relative_to(root).as_posix(),
                 'bytes': i['bytes'], 'sha256': i['sha256']} for i in items]
    relative.sort(key=lambda item: item['path'])
    data = json.dumps(relative, sort_keys=True, separators=(',', ':')).encode('utf-8')
    return hashlib.sha256(data).hexdigest()


def _cache(build):
    path = build / 'CMakeCache.txt'
    require(path.is_file(), 'CMake cache missing: ' + str(path))
    values = {}
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if line.startswith(('#', '//')) or not line.strip():
            continue
        match = re.fullmatch(r'([^:=]+):[^=]*=(.*)', line)
        if match:
            key, value = match.groups()
            require(key not in values, 'duplicate CMake cache entry: ' + key)
            values[key] = value
    return values


def _cache_path(cache, key):
    require(bool(cache.get(key)), 'CMake cache entry missing: ' + key)
    path = Path(cache[key])
    require(path.is_absolute(), 'CMake cache path must be absolute: ' + key)
    return path.resolve()


def _configuration(build):
    cache = _cache(build)
    entry = (ROOT / 'adapters/llama_cpp/integrated').resolve()
    require(_cache_path(cache, 'CMAKE_HOME_DIRECTORY') == entry,
            'CMake cache source entry is not the integrated model entry')
    require(cache.get('CMAKE_PROJECT_NAME') == 'realgpu_llama',
            'CMake cache project is not the integrated model')
    require(_cache_path(cache, 'CMAKE_CACHEFILE_DIR') == build,
            'CMake cache build location changed; rebuild required')
    backend = _cache_path(cache, 'HBFLAB_EVENTLAB_SOURCE')
    # project(HBFEventLab) records the directory actually added by CMake.  Merely
    # trusting an overridden HBFLAB_EVENTLAB_SOURCE cache value is insufficient.
    require(_cache_path(cache, 'HBFEventLab_SOURCE_DIR') == backend,
            'CMake cache backend mismatch: HBFLAB_EVENTLAB_SOURCE != HBFEventLab_SOURCE_DIR')
    for key, expected in (('realgpu_llama_SOURCE_DIR', entry),
                          ('HBFEventLab_BINARY_DIR', build / 'eventlab'),
                          ('llama.cpp_SOURCE_DIR', (ROOT / 'third_party/llama.cpp-integrated').resolve())):
        if key in cache:
            require(_cache_path(cache, key) == expected, 'CMake cache location mismatch: ' + key)
    require(cache.get('LLAMA_BUILD_COMMIT') == FRAMEWORK_COMMIT,
            'CMake cache framework commit mismatch')
    return entry, backend


def _framework():
    root = (ROOT / 'third_party/llama.cpp-integrated').resolve()
    overlay_path = root / 'hbflab-overlay.json'
    overlay = read(overlay_path)
    require(isinstance(overlay, dict) and overlay.get('upstream') == FRAMEWORK_COMMIT,
            'framework upstream commit mismatch')
    modified = overlay.get('modified_sha256')
    require(isinstance(modified, dict) and bool(modified), 'framework overlay inventory missing')
    for name, digest in modified.items():
        path = root / name
        require(not Path(name).is_absolute() and path.resolve().is_relative_to(root),
                'framework overlay path escapes root: ' + name)
        require(path.is_file(), 'framework overlay file missing: ' + name)
        require(sha(path) == digest, 'framework overlay changed: ' + name)
    return {'commit': FRAMEWORK_COMMIT, 'path': str(root),
            'overlay_sha256': sha(overlay_path), 'modified_sha256': modified}


def _artifacts(build):
    require(executable(build / 'bin', 'realgpu_llama').is_file(), 'model executable missing')
    require((build / 'CMakeCache.txt').is_file(), 'CMake cache missing')
    # Include nested runtime helpers/libraries and every generated root PTX.
    # Recomputing this inventory also rejects newly added runtime artifacts.
    paths = [p for p in (build / 'bin').rglob('*') if p.is_file()]
    paths += list(build.glob('*.ptx')) + [build / 'CMakeCache.txt']
    for path in paths:
        require(path.resolve().is_relative_to(build), 'artifact path escapes build: ' + str(path))
    return records(paths)


def _git_head(root):
    """Optional local provenance only: do not inspect remotes/config or credentials."""
    try:
        # A real file avoids captured-stdio/named-pipe restrictions on Windows.
        with tempfile.TemporaryFile() as output:
            result = subprocess.run(['git', '-C', str(root), 'rev-parse', '--verify', 'HEAD'],
                                    stdout=output, stderr=subprocess.DEVNULL, timeout=10, check=False)
            output.seek(0)
            value = output.read().decode('ascii', errors='replace').strip()
        if result.returncode == 0 and re.fullmatch(r'[0-9a-fA-F]{40}|[0-9a-fA-F]{64}', value):
            return value.lower()
    except (OSError, subprocess.TimeoutExpired):
        pass
    return None


def seal(build):
    build = Path(build).resolve()
    path = build / 'release-identity.json'
    require(not path.exists(), 'sealed build cannot be overwritten')
    entry, backend_path = _configuration(build)
    backend_sources = backend_inputs(backend_path)
    backend_hash = _source_hash(backend_path, backend_sources)
    sources = inputs()
    value = {
        'schema_version': SCHEMA_VERSION, 'status': 'sealed',
        'build_directory': str(build), 'source_entry': str(entry),
        'frontend': {'path': str(ROOT.resolve()), 'source_sha256': _source_hash(ROOT, sources)},
        'backend': {
            # Legacy predeclared/manifest consumers expect this string key.
            'commit': 'sha256:' + backend_hash, 'identity_kind': 'source-inventory-sha256',
            'source_sha256': backend_hash, 'baseline_commit': BASELINE_COMMIT,
            'path': str(backend_path), 'sources': backend_sources,
            'git_head_at_seal': _git_head(backend_path),
        },
        'framework': _framework(), 'sources': sources, 'artifacts': _artifacts(build),
        'provenance': {
            'monorepo_git_head_at_seal': _git_head((ROOT / '../..').resolve()),
            'git_head_policy': 'provenance-only; verify source hashes, not current HEAD',
            'baseline_policy': 'phase-1 reference only; not the current backend identity',
        },
    }
    # Exclusive creation also prevents concurrent seals from replacing one another.
    with path.open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(value, indent=2) + '\n')
    return value


def verify_build(build=DEFAULT_BUILD, expected=None):
    """Verify live inventories. expected is an optional source SHA-256, not Git HEAD."""
    build = Path(build).resolve()
    value = read(build / 'release-identity.json')
    require(isinstance(value, dict) and value.get('schema_version') == SCHEMA_VERSION and
            value.get('status') == 'sealed', 'unsupported build identity; rebuild and seal required')
    require(value.get('build_directory') == str(build), 'build location changed; rebuild required')
    entry, backend_path = _configuration(build)
    require(value.get('source_entry') == str(entry), 'model source entry changed; rebuild required')
    frontend = value.get('frontend', {})
    require(frontend.get('path') == str(ROOT.resolve()), 'frontend location changed; rebuild required')
    backend = value.get('backend', {})
    require(backend.get('path') == str(backend_path), 'backend location changed; rebuild required')
    require(backend.get('baseline_commit') == BASELINE_COMMIT and
            backend.get('identity_kind') == 'source-inventory-sha256', 'backend identity metadata mismatch')
    backend_sources = backend_inputs(backend_path)
    require(backend.get('sources') == backend_sources, 'backend source inventory changed; rebuild required')
    backend_hash = _source_hash(backend_path, backend_sources)
    identity = 'sha256:' + backend_hash
    require(backend.get('commit') == identity and backend.get('source_sha256') == backend_hash,
            'backend source identity mismatch')
    require(expected is None or expected in (identity, backend_hash),
            'expected backend mismatch: use the actual source SHA-256, not a baseline/Git commit')
    sources = inputs()
    require(value.get('sources') == sources and frontend.get('source_sha256') == _source_hash(ROOT, sources),
            'frontend source inventory changed; rebuild required')
    require(value.get('framework') == _framework(), 'framework identity changed; rebuild required')
    require(isinstance(value.get('artifacts'), list), 'artifact inventory missing')
    verify_records(value['artifacts'])
    require(value['artifacts'] == _artifacts(build), 'artifact inventory changed; rebuild required')
    # Return the original snapshot so runners can compare identities before/after
    # execution even if an unrelated documentation commit advances HEAD meanwhile.
    return value


def add_build_arguments(parser):
    parser.add_argument('--cuda-root', type=Path)
    parser.add_argument('--gpu-device', default='0')
    parser.add_argument('--build-dir', type=Path, default=DEFAULT_BUILD)
    parser.add_argument('--expected-backend', default=None,
                        help='actual backend source SHA-256 (with optional sha256: prefix); '
                             'default verifies the sealed source hash, not the baseline/Git HEAD')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['seal', 'verify'])
    add_build_arguments(parser)
    args = parser.parse_args()
    result = seal(args.build_dir) if args.action == 'seal' else verify_build(args.build_dir, args.expected_backend)
    print(result['status'])
