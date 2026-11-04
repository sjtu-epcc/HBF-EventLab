# Real GPU integration baseline

> **2026-11-04 update:** the separate S-series minimum experiment now builds and passes real CUDA/new-backend acceptance on the local RTX 4070 Laptop GPU. It reuses the explicitly identified upstream RTX 5070 compute table; it is not a new local compute-time measurement or a calibrated HBF prediction. See [S integration results and reproduction](gpu_s_integration.md). The 2026-11-03 results below remain historical; the two async frontend overlap failures are not closed by the S experiment.

## Branches and status

- `main`: backend baseline `8ccb1f353630f893f761b017d621812b7b9d5bf2`, including incremental sessions, resident budgets, guarded fault injection and Host/runtime optimizations.
- `feat/real-gpu-integration`: imports [ZeePgm/RealGPUAcess_v0 at 41b9f736](https://github.com/ZeePgm/RealGPUAcess_v0/tree/41b9f736164c8517c35aaafb6743a803bf656257) into [integrations/real_gpu](../integrations/real_gpu/README.md).
- The integration builds the current repository backend directly. There is **no second `third_party/HBF-EventLab` tree** and no replay of the old six-file backend overlay.
- This is a **functional collaboration baseline, not a predictive modeled TTFT/TPOT release**. Real inference data lives in CUDA allocations/capacity backing; HBF data identities are symbolic.
- The [old Qwen tensor-callback experiment](../experiments/qwen_online/CMakeLists.txt) remains optional and disabled by default. It is not used by the new frontend. Removing it is deferred.

The [import record](../integrations/real_gpu/integration-origin.json) identifies upstream files by SHA-256 and records the backend baseline. Upstream release manifests, validation records and old backend-overlay files are retained as **historical evidence**, not relabeled as tests of this integration.

## Layout and dependencies

The repository root owns `src`, `include`, `tests`, and the default backend CMake build. The frontend has a separate CMake entry under `integrations/real_gpu`; its model entry is `integrations/real_gpu/adapters/llama_cpp/integrated`. Neither is added as a subdirectory by the root build.

Both frontend entries default `HBFLAB_EVENTLAB_SOURCE` to the repository root. Override it explicitly only when intentionally building another backend; model build identity verifies the actual cache path and source files. Do not use symlinks or manually replace libraries in a sealed build.

- C++17, CMake >=3.24 for the frontend, VS2022 on Windows.
- Python >=3.11 for frontend tools (`hashlib.file_digest`), with packages from [requirements.txt](../integrations/real_gpu/requirements.txt). Backend Python regression tests need >=3.10.
- CUDA toolkit, compatible NVIDIA GPU/driver; the model entry additionally requires CUPTI/cuBLAS.
- Pinned llama.cpp `b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9` with the seven-file frontend overlay. The bundled framework includes tokenizer/test GGUF fixtures, **not a complete inference model**.
- Model inference requires external F16 GGUF, a matching conversion record, frozen prompt/token IDs and a locally derived profile. Do not commit models, profiles, machine configuration, build products or traces.

## Build and test

Commands below run from the **HBF-EventLab repository root** in a VS2022 x64 developer shell. Use a fresh directory per configuration. Set the tool paths and architecture for your machine; `89-real` is the RTX 4070 Laptop target used for this integration check, not a universal default.

### Backend plus frontend CPU

```powershell
cmake -S integrations/real_gpu -B build-real-gpu-cpu -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DHBFLAB_ENABLE_CUDA=OFF `
  -DHBFLAB_BACKEND_TESTS=ON -DBUILD_TESTING=ON `
  -DHBF_EVENTLAB_EXPERIMENT_TESTS=ON -DPython3_EXECUTABLE=<absolute-python-path>
cmake --build build-real-gpu-cpu --parallel 2
ctest --test-dir build-real-gpu-cpu --output-on-failure
python -B integrations/real_gpu/tests/platform_environment_test.py
python -B integrations/real_gpu/tests/integrated_validation_contract.py
python -B integrations/real_gpu/tests/integrated_build_identity_test.py
```

This includes the backend Python tests, not only the C++ subset. The three frontend Python scripts are separate checks, not registered in the imported CTest suite.

### GPU runtime (no model weights or process injection)

```powershell
cmake -S integrations/real_gpu -B build-real-gpu-cuda -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DHBFLAB_ENABLE_CUDA=ON `
  -DHBFLAB_ENABLE_INJECTION=OFF -DHBFLAB_BACKEND_TESTS=OFF -DBUILD_TESTING=ON `
  "-DCUDAToolkit_ROOT=$env:CUDA_PATH" `
  "-DCMAKE_CUDA_COMPILER=$env:CUDA_PATH/bin/nvcc.exe" `
  -DCMAKE_CUDA_ARCHITECTURES=89-real -DHBFLAB_PTX_ARCHITECTURE=compute_75
cmake --build build-real-gpu-cuda --parallel 2
ctest --test-dir build-real-gpu-cuda --output-on-failure
```

This entry runs frontend CPU/GPU contracts; backend tests have already been selected in the CPU build. Process-injection checks are **not** covered when disabled.

### llama.cpp integrated model entry

```powershell
cmake -S integrations/real_gpu/adapters/llama_cpp/integrated -B build-real-gpu-model -G Ninja `
  -DCMAKE_BUILD_TYPE=Release "-DCUDAToolkit_ROOT=$env:CUDA_PATH" `
  "-DCMAKE_CUDA_COMPILER=$env:CUDA_PATH/bin/nvcc.exe" `
  -DCMAKE_CUDA_ARCHITECTURES=89-real -DHBFLAB_PTX_ARCHITECTURE=compute_75
cmake --build build-real-gpu-model --parallel 2
python integrations/real_gpu/tools/integrated_build_identity.py seal --build-dir build-real-gpu-model
python integrations/real_gpu/tools/integrated_build_identity.py verify --build-dir build-real-gpu-model
```

The model entry builds operator executables but does **not** register them in CTest. A successful configure/build is not inference validation. If CUPTI is installed separately, pass `-DHBFLAB_CUPTI_ROOT=<sdk-root>` to CMake (or `-CuptiRoot` / `cupti_root` to the wrapper); runtime helpers use the sealed cache path. The [NVIDIA CUDA redistributable manifest](https://developer.download.nvidia.com/compute/cuda/redist/redistrib_12.9.1.json) identifies the optional matching SDK. A complete cuBLAS development installation is still required. Generate a profile using the imported [model instructions](../integrations/real_gpu/README.md#model-inputs-and-execution), then run:

```powershell
python integrations/real_gpu/tools/run_llama_integrated.py `
  --build-dir build-real-gpu-model --profile <local-profile.json> `
  --prompt integrations/real_gpu/adapters/llama_cpp/prompt.txt --tokens 32 `
  --output <fresh-output-directory>
```

The runner checks native-KV reference, paged bypass and online modeled modes. The native reference is controlled/customized, not unmodified stock llama.cpp. Exact supported scope and predeclared numerical contracts remain enforced by the profile generator.

## Build identity and source distribution

Model identity records the **actual backend path and source digest**, frontend/framework inputs, CMake cache and produced runtime artifacts. The historical backend baseline commit and the monorepo Git HEAD at seal time are provenance fields, not substitutes for content verification. In this integration, `backend.commit` is a `sha256:` content identity (kept under the old key for runner compatibility), not a claim that the backend is still commit `72e0745`.

After changing source or build paths, rebuild and reseal in a fresh directory. Never edit an old identity to bless changed artifacts. Local build manifests contain absolute paths and remain untracked.

The old standalone `package_source.py create` is disabled: the frontend subdirectory alone no longer contains the backend and is not a portable package. Publish the repository branch, or use `git archive` from a committed repository root. Historical archive verification remains available; it does not certify this integration.

## Validation of this integration

Fresh checks on 2026-11-03 used MSVC 19.43 x64, CUDA 12.9.86, NVIDIA RTX 4070 Laptop GPU (8GB), driver 596.21. Backend Python tests used Python 3.14; frontend Python tools used Python 3.12 with NumPy 2.3.5 and PyYAML 6.0.3. See the [compact validation record](validation/gpu-integration-20261103.json).

| Check | Result | Scope / limitations |
|---|---|---|
| CPU combined Release suite | **604/604 passed**, 0 skipped, 83.63s | 512 backend tests (including Python) plus 92 frontend CPU contracts |
| Frontend CPU/CUDA suite, injection disabled | **220/222 passed; 2 failed**, 54.21s | 92 CPU + 130 GPU cases; NOT full GPU acceptance |
| Focused GPU smoke | 6 cases, each passed 3 consecutive runs | Async serial/identity, PTX control/wait, capacity writeback/KV; does not replace full-suite results |
| Python environment / validator / identity | **9 + 5 + 25 passed** | Identity tests use temporary fixtures, not a successfully built full-model executable |
| Wrapper/path precedence and archive behavior | 23 wrapper assertions; archive refusal/legacy verify passed | No silent rewrite of old source manifests |
| Full llama.cpp model executable / model operator suite / inference | **Not built / not run** | Local CUDA installation lacks cuBLAS development headers/libraries; no complete F16 GGUF + conversion record supplied |
| Process injection; Linux; CUDA13 machine | Not tested in this integration | Historical upstream evidence is separate |

Known failures and adaptations are preserved:

- The first CUDA build exposed the CUDA13 vs CUDA12 `cuCtxCreate` signature difference in a context-isolation test. A version guard preserves the same test behavior on both toolkits; no production protocol changed.
- The first full run was 221/222: `lab_coop_max_threads` hit the default 0.5s per-request software watchdog. Three unchanged isolated runs produced one pass and two timeouts. Only this 256-slot stress fixture now selects the Runtime's existing 1s supported limit; production defaults, the deliberate 5ms timeout case, request counts and correctness assertions are unchanged. The adjusted case passed three consecutive checks and the later full run. This is a test budget adaptation, not a performance improvement claim.
- The later full run failed `lab_shared_gpu_owner` and `lab_shared_gpu_scaled` at `no GPU/media overlap exercised`. Each subsequently passed three isolated runs with the original overlap assertion. The fixture does not deterministically rendezvous an admitted GPU request with an active media interval; the statistic samples outstanding backend requests, not merely a live GPU kernel. This supports a scheduling-sensitive coverage precondition, but **does not establish that a production overlap bug is absent**. The failures remain open; do not replace 220/222 with the isolated pass count or remove the assertion.
- Two stale hashes in the imported framework overlay metadata were reconciled against the unchanged upstream source, original release manifest and import inventory. The original overlay metadata is retained; no framework CUDA implementation was changed.
- Standalone CUPTI 12.9.79 was obtained locally from NVIDIA's official redistributable (verified SHA-256) to diagnose the model build. It is not redistributed in Git. The remaining cuBLAS dependency is explicitly rejected at configure time; no real model seal or model-run result is claimed.

The backend publication was independently rebuilt and passed 512/512 tests in both Debug and Release on 2026-11-02, before this frontend was added. See [backend verification](verification.md). The stable `main` is unchanged by these integration limitations.

## Independent event-model work still required

The existing frontend maps real GPU/host readiness to simulated arrivals and waits for a CPU service thread. CPU execution speed can therefore influence subsequent simulated input times. `process_wall_ms`, final backend time and sums of individual access delays are **not** predictive token latency.

For modeled TTFT/TPOT, implement and validate separately:

1. A logical compute/request timeline independent of CPU simulator wall time, with measured compute inputs bound to model, GPU, precision, input and execution position.
2. Explicit causal dependencies and supported overlap assumptions. Do not simply sum all concurrent access latencies or subtract CPU wall time after the run.
3. Request start, prefill completion, token-ready and EOS events, including compute tails after the last modeled access. Define treatment of sampling, required transfers and initialization; final drain is separate.
4. Clear access coverage and replacement/extra-gating assumptions: only selected norm/down weight ranges and paged KV are modeled, not all model weights. Avoid double-counting the original GPU memory work.
5. Invariance checks: delaying the CPU service or enabling diagnostics must not change predicted times for fixed logical inputs. Equivalent backend optimizations must change software cost, not predictions.
6. Numerical, dependency-success, budget and close/drain validation. A failed or incomplete run cannot contribute a successful latency sample.

Then use `TTFT=t1-t0` and `TPOT=(tN-t1)/(N-1)` with declared token-ready semantics and null values when N is insufficient. The imported [experiment design](../integrations/real_gpu/docs/GPU_EXPERIMENT_DESIGN.md) is a proposal, not an implemented executor or a requirement to restore the old Qwen frontend.

## License and attribution

The project maintainer confirmed permission to redistribute the original frontend and overlay contributions under Apache-2.0. See [frontend notices](../integrations/real_gpu/THIRD_PARTY_NOTICES.md) and the repository [license](../LICENSE). Upstream llama.cpp, Microsoft Detours and their dependencies retain their existing licenses and attribution. Import provenance is retained without claiming that the original public repository had already declared an Apache license.
