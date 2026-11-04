"""Bind S experiment sources/binaries to the actual sealed dependency/backend.

The existing model seal deliberately excludes experiments. This complementary
seal closes that gap; Git labels alone are never treated as build identity.
"""
import argparse
import json
import sys
from pathlib import Path
from common import sha

SOURCE = Path(__file__).resolve().parent
FRONTEND = SOURCE.parents[1]
sys.path.insert(0, str(FRONTEND/'tools'))
from integrated_build_identity import verify_build as verify_dependency, backend_inputs, _source_hash, _cache, records, require
from platform_environment import executable


def snapshot(build):
    build = Path(build).resolve()
    cache = _cache(build)
    require(Path(cache['CMAKE_HOME_DIRECTORY']).resolve() == SOURCE, 'Wrong S experiment source entry')
    require(Path(cache['CMAKE_CACHEFILE_DIR']).resolve() == build, 'S build location changed')
    require(cache['CMAKE_PROJECT_NAME'] == 'gpu_s_stages', 'Wrong S project')
    backend = Path(cache['HBFLAB_EVENTLAB_SOURCE']).resolve()
    require(Path(cache['HBFEventLab_SOURCE_DIR']).resolve() == backend, 'Actual S backend source mismatch')
    base = Path(cache['BASE_BUILD']).resolve()
    dependency = verify_dependency(base)
    sources = backend_inputs(backend)
    require(dependency['backend']['path'] == str(backend), 'S/model backend paths differ')
    require(dependency['backend']['source_sha256'] == _source_hash(backend, sources), 'S/model backend source identities differ')
    # Exact imported dependencies must be inside the sealed base build.
    imported = []
    for name in ('llama', 'ggml', 'ggml-base'):
        path = Path(cache['S_LIBRARY_'+name]).resolve()
        require(path.is_relative_to(base) and path.is_file(), 'Imported dependency outside selected build')
        imported.append(path)
    for name in ('gpu_s_native', 'gpu_s_s8', 'gpu_s_bridge'):
        require(executable(build/'bin', name).is_file(), 'Required S executable missing: '+name)
    inputs = [p for p in SOURCE.rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix not in ('.pyc', '.pyo')]
    artifacts = [p for p in (build/'bin').rglob('*') if p.is_file()]
    artifacts += [build/'CMakeCache.txt']
    artifacts += [p for p in (build/'eventlab').glob('*') if p.is_file() and p.suffix in ('.lib', '.a')]
    sdk = []
    cupti = Path(cache['S_CUPTI_INCLUDE'])
    sdk += [p for p in cupti.glob('*.h') if p.is_file()]
    sdk.append(Path(cache['S_CUPTI_LIBRARY']))
    roots = [Path(cache.get('HBFLAB_CUPTI_ROOT') or cupti.parent)]
    base_cache = _cache(base)
    # integrated.py intentionally loads the dependency build's runtime paths.
    # Require the S compiler/linker inputs to select those exact same SDKs.
    for own, parent in (('CUDAToolkit_ROOT', 'CUDAToolkit_ROOT'),
                        ('CUDA_cudart_LIBRARY', 'CUDA_cudart_LIBRARY'),
                        ('S_CUPTI_INCLUDE', 'M2_CUPTI_INCLUDE'),
                        ('S_CUPTI_LIBRARY', 'M2_CUPTI_LIBRARY')):
        require(bool(cache.get(own)) and bool(base_cache.get(parent)), 'SDK configuration missing: '+own)
        require(Path(cache[own]).resolve() == Path(base_cache[parent]).resolve(), 'S/base runtime SDK mismatch: '+own)
    cuda = Path(base_cache['CUDAToolkit_ROOT']).resolve()
    roots.append(cuda)
    sdk.append(Path(base_cache['CUDA_cudart_LIBRARY']))
    cublas = base_cache.get('CUDA_cublas_LIBRARY')
    if cublas:
        path = Path(cublas); sdk.append(path)
        roots.append(path.parent.parent.parent if path.parent.name == 'x64' else path.parent.parent)
    for key in ('CUDA_cublasLt_LIBRARY', 'CUDA_cuda_driver_LIBRARY'):
        value = base_cache.get(key)
        if value and not value.endswith('-NOTFOUND'):
            sdk.append(Path(value))
    for root in roots:
        sdk += [p for p in (root/'include').rglob('*') if p.is_file() and p.suffix in ('.h', '.hpp', '.cuh')]
        for folder in ('bin', 'bin/x64', 'lib', 'lib/x64', 'lib64'):
            sdk += [p for p in (root/folder).glob('*') if p.is_file() and (p.suffix == '.dll' or '.so' in p.name or p.suffix == '.dylib')]
    for path in sdk:
        require(path.is_file(), 'Configured SDK input missing: '+str(path))
    return {'schema': 1, 'status': 'sealed', 'build': str(build), 'source': str(SOURCE),
            'base_build': str(base), 'dependency_identity_sha256': sha(base/'release-identity.json'),
            'backend_source_sha256': dependency['backend']['source_sha256'],
            'sources': records(inputs), 'artifacts': records(artifacts),
            'imported_libraries': records(imported), 'external_SDK_inputs': records(sdk)}


def seal(build):
    build = Path(build).resolve()
    require(not (build/'experiment-identity.json').exists(), 'Do not overwrite a sealed experiment build')
    value = snapshot(build)
    with (build/'experiment-identity.json').open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
    return value


def verify(build):
    build = Path(build).resolve()
    value = json.loads((build/'experiment-identity.json').read_text(encoding='utf-8'))
    require(value == snapshot(build), 'Experiment source, configuration, library or executable changed; rebuild/reseal in a new evidence context')
    return value


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['seal', 'verify'])
    p.add_argument('--build', type=Path, required=True)
    a = p.parse_args()
    r = seal(a.build) if a.action == 'seal' else verify(a.build)
    print(r['status'], r['backend_source_sha256'])
