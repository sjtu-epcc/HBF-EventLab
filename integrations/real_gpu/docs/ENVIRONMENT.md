# Environment configuration

Windows x64 with an NVIDIA CUDA GPU is the validated platform family. GPU target,
tool locations and output directories are configuration inputs. Host/Device,
content identities, scheduling and metric definitions do not depend on them.

## Windows machine settings

In this integrated tree, `backend_source` defaults to the HBF-EventLab repository
root (`../..` from the frontend directory). `-BackendSource` overrides it.
A relative backend path in JSON is resolved against the JSON directory; a command-line
path is resolved against the calling directory. The actual selected backend is
recorded and verified by the model build identity, not inferred from old manifests.
`-EnableInjection OFF` (or `enable_injection: "OFF"`) explicitly disables Windows
process-injection targets for ordinary CUDA contract testing. Model builds can
also select a standalone SDK using `-CuptiRoot` / `cupti_root`; this becomes
`HBFLAB_CUPTI_ROOT` in CMake and is used by runtime-library discovery. Its relative
path rules match backend_source. This does not replace the cuBLAS development
headers/libraries required from a complete CUDA toolkit.

Copy `configs/machine.example.json` to `configs/machine.local.json`. The local
file is ignored by Git. For example:

```json
{
  "cuda_root": "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.0",
  "cuda_architectures": "120a-real",
  "ptx_architecture": "compute_75",
  "python": "../.venv/Scripts/python.exe",
  "build_dir": "../build-model-machine",
  "jobs": 6
}
```

Paths in a JSON file are relative to that file; explicit command-line paths are
relative to the working directory. Bare commands such as `python` use PATH.
Optional `vs_root`, `cmake` and `ninja` select installations explicitly. If omitted,
VS2022 is discovered with vswhere, and its bundled CMake/Ninja are used.
CUDA root defaults to CUDA_PATH, architectures to `native`, and PTX to `compute_75`.
Explicit command-line options override JSON, which overrides defaults/environment.
Unknown JSON keys and malformed architecture settings are rejected.

```powershell
./build.ps1 -Target model -Config ./configs/machine.local.json -PrintConfiguration
./build.ps1 -Target model -Config ./configs/machine.local.json
# Equivalent command-line configuration:
./build.ps1 -Target cuda -CudaRoot 'C:/CUDA/v13.0' -CudaArchitectures '89-real' -BuildDir ./build-cuda-new
```

`-PrintConfiguration` only resolves settings; it does not check tool availability
or build. `cuda_architectures` accepts CMake's native/all/all-major or an explicit
semicolon-separated target list. Use an explicit architecture for reproducible
builds; native detects the build machine. `ptx_architecture` selects the virtual
architecture of embedded PTX, independently of compiled CUDA targets. The toolkit
and GPU must support both choices; changing the target is not proof of compatibility.

Use different build directories for cpu/cuda/model. Sealed model builds cannot be
overwritten. Changed sources or machine paths require a fresh build and seal;
build integrity checks remain enabled. No model weights or generated profiles
are moved automatically.

## Runtime

Model and operator runners accept `--cuda-root` and `--gpu-device`. The matrix
runner forwards them to each model run. CUDA lookup is explicit option, then the
sealed build's CUDAToolkit_ROOT, then CUDA_PATH. Runtime library directories are
added only if present. Device selection uses CUDA_VISIBLE_DEVICES; the model
still sees one logical device and retains its single-GPU contract.

```powershell
python tools/run_llama_integrated.py --build-dir ./build-model-machine --gpu-device 1 --profile ./configs/integrated/model.json --output ./output/machine-test
```

Regenerate local model profiles after relocation. Revalidate numerical results,
access coverage and dependent GPU waits after a GPU/toolchain change. GPU compute
tables must be remeasured for the new environment; old tables remain valid only
as explicitly attributed fixed inputs to backend comparisons.

## Platform operations and other operating systems

`include/hbflab/platform.hpp` isolates environment updates, aligned CUPTI buffers
and available host-memory queries. It provides Windows and POSIX implementations;
runner code uses this interface. Python executable names and library paths are
isolated in `tools/platform_environment.py`. Available-memory queries do not
account for every container or job-object memory limit.

`build.ps1` remains the Windows entry. On Linux use CMake directly with the same
configuration keys, for example:

```sh
cmake -S adapters/llama_cpp/integrated -B build-model-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCUDAToolkit_ROOT=/opt/cuda \
  -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
  -DCMAKE_CUDA_ARCHITECTURES=89-real -DHBFLAB_PTX_ARCHITECTURE=compute_75
cmake --build build-model-linux --parallel 6
python tools/integrated_build_identity.py seal --build-dir build-model-linux
```

Linux support is not certified by the Windows checks. Windows process injection
remains platform-specific, and CUDA/CUPTI version compatibility still requires
validation. This layer does not implement an AMD backend or extend supported
model architectures.
