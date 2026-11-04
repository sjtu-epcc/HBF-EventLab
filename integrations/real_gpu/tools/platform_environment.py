"""Platform-dependent executable names and runtime library discovery."""
import os
from pathlib import Path


def executable(folder, name):
    return Path(folder) / (name + ('.exe' if os.name == 'nt' else ''))


def cache_value(build, name):
    cache = Path(build) / 'CMakeCache.txt'
    if cache.is_file():
        for line in cache.read_text(encoding='utf-8').splitlines():
            if line.startswith(name + ':') and '=' in line:
                return line.split('=', 1)[1]
    return None


def runtime_environment(build, source_root, cuda_root=None, gpu_device='0'):
    build, source_root = Path(build).resolve(), Path(source_root).resolve()
    root = cuda_root or cache_value(build, 'CUDAToolkit_ROOT') or os.environ.get('CUDA_PATH')
    env = dict(os.environ)
    cuda = Path(root).resolve() if root else None
    if cuda and not cuda.is_dir():
        raise ValueError('CUDA runtime root does not exist: ' + str(cuda))
    cupti_root = cache_value(build, 'HBFLAB_CUPTI_ROOT')
    cupti = Path(cupti_root).resolve() if cupti_root else None
    if cupti and not cupti.is_dir():
        raise ValueError('CUPTI runtime root does not exist: ' + str(cupti))

    # Frontend builds can live beside integrations/ at the backend root. Prefer
    # the configured backend when using an external checkout instead.
    backend = cache_value(build, 'HBFLAB_EVENTLAB_SOURCE')
    build_roots = [source_root]
    if source_root.parent.name == 'integrations':
        build_roots.append(source_root.parent.parent)
    if backend:
        build_roots.append(Path(backend).resolve())

    def inherited_paths(value):
        result = []
        for entry in value.split(os.pathsep):
            if not entry:
                continue
            path = Path(entry).resolve()
            # A toolkit may itself be installed under a directory named build-*.
            if cuda and path.is_relative_to(cuda):
                result.append(entry)
                continue
            stale = False
            for parent in build_roots:
                if path.is_relative_to(parent):
                    relative = path.relative_to(parent)
                    if relative.parts and relative.parts[0].startswith('build-'):
                        stale = True
                        break
            if not stale:
                result.append(entry)
        return result

    inherited = inherited_paths(env.get('PATH', ''))
    libraries = [build / 'bin']
    # Explicit SDK paths are trusted build inputs, even under backend/build-*.
    # Prefer them over a different CUPTI version bundled in the CUDA toolkit.
    if cupti:
        libraries += [cupti / p for p in ('bin', 'bin/x64', 'lib', 'lib/x64', 'lib64')]
    # NVIDIA redistributable cuBLAS may be outside the nvcc installation.
    # Resolve only the library chosen by CMake, never an unrelated SDK on PATH.
    cublas_library = cache_value(build, 'CUDA_cublas_LIBRARY')
    if cublas_library and not cublas_library.endswith('-NOTFOUND'):
        chosen = Path(cublas_library).resolve()
        if not chosen.is_file():
            raise ValueError('Configured cuBLAS library does not exist: ' + str(chosen))
        libdir = chosen.parent
        sdk = libdir.parent.parent if libdir.name == 'x64' else libdir.parent
        libraries += [sdk / 'bin', sdk / 'bin/x64', libdir]
    if cuda:
        env['CUDA_PATH'] = str(cuda)
        libraries += [cuda / p for p in ('bin', 'bin/x64', 'lib', 'lib/x64',
                                         'lib64', 'extras/CUPTI/lib64', 'extras/CUPTI/lib/x64')]
    libraries = [str(p) for p in libraries if p.is_dir()]
    env['PATH'] = os.pathsep.join(libraries + inherited)
    if os.name != 'nt':
        env['LD_LIBRARY_PATH'] = os.pathsep.join(libraries + inherited_paths(env.get('LD_LIBRARY_PATH', '')))
    env['CUDA_VISIBLE_DEVICES'] = str(gpu_device)
    env['GGML_CUDA_DISABLE_GRAPHS'] = '1'
    for key in ('GGML_CUDA_CUBLAS_COMPUTE_TYPE', 'GGML_CUDA_DISABLE_FUSION',
                'HBFLAB_STORAGE_CONFIG', 'HBFLAB_M6_NORM'):
        env.pop(key, None)
    return env
