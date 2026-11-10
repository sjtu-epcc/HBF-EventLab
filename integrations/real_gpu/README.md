# Real GPU experiments

The optional GPU frontend uses the HBF EventLab backend at the repository root.
GPU execution owns real weight bytes; the backend models symbolic content,
resource use and access completion through its existing incremental Host.

| Experiment | Purpose | Entry |
|---|---|---|
| S fixed activity trace | Selected ranges, dependency completion and token-time conservation | [S instructions](experiments/gpu_s/README.md) |
| F1–F3 FFN coverage | Compare 1, 4 and 28 FFN layers using one frozen GPU activity table | [F instructions](experiments/gpu_ffn/README.md) |
| Asynchronous / paged KV | Runtime, finite frames and mutable snapshot contracts | Model commands below |

S/F token times combine a frozen GPU activity table with online HBF access latency.
The asynchronous runtime uses a different time mapping; its software wall time
is reported separately. [Validation scope](docs/VALIDATION.md) describes these boundaries.

## Backend and dependencies

CMake links the backend from this repository; `HBFLAB_EVENTLAB_SOURCE` can select
an explicit checkout. The [incremental adapter contract](docs/INCREMENTAL_BACKEND.md)
describes ownership and completion. [Import provenance](integration-origin.json)
and dependency license notices preserve upstream attribution.

## Environment and build

Use CMake >=3.24, a C++17 compiler, Python >=3.11 and a compatible CUDA toolkit/GPU. Model builds additionally require CUPTI/cuBLAS. Select the architecture supported by your GPU and toolkit.

Commands in this README run from **this `integrations/real_gpu` directory**.

```powershell
python -m venv .venv
./.venv/Scripts/python.exe -m pip install -r requirements.txt
./build.ps1 -Target cpu -Python (Resolve-Path './.venv/Scripts/python.exe').Path
ctest --test-dir ./build-cpu --output-on-failure
# Set CUDA_PATH and replace 89-real for your GPU/toolkit.
./build.ps1 -Target cuda -CudaArchitectures 89-real -EnableInjection OFF -Python (Resolve-Path './.venv/Scripts/python.exe').Path
ctest --test-dir ./build-cuda --output-on-failure
./build.ps1 -Target model -CudaArchitectures 89-real -Python (Resolve-Path './.venv/Scripts/python.exe').Path
```

The wrapper builds but does not run CTest. Model executables require separate operator/model validation; they are not registered in CTest. Model builds seal the actual backend/frontend/framework source inventory and artifacts. Use a fresh build directory after source/path changes. See [environment settings](docs/ENVIRONMENT.md).

## Model inputs and execution

Provide a supported F16 GGUF and conversion record with `gguf_sha256`, `llama_commit` and source revision. The required framework commit is `b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9`; the bundled framework also includes the existing frontend overlay. Model weights are external inputs; bundled GGUF vocabulary fixtures are not inference models.

```powershell
python tools/integrated_profile.py --model ./models/model.gguf --conversion ./models/conversion.json --output ./configs/integrated/model.json
python tools/run_llama_integrated.py --profile ./configs/integrated/model.json --prompt ./adapters/llama_cpp/prompt.txt --tokens 32 --output ./output/example
```

The runner executes native-KV reference, paged bypass and online modeled modes. It checks complete logits, tokens, selected access coverage, GPU waits, KV publication, Host final outcomes, resident budgets and final close/drain. Use a fresh output directory. Profiles are rederived from GGUF metadata and hashes; editing them cannot weaken the checks. Generated local profiles/paths remain untracked.

For the full independent operator script, supply profiles named `qwen3-0.6b.json`, `qwen3-1.7b.json` and `smollm2-135m.json` in `configs/integrated`, then use `tools/check_llama_integrated.py`. The optional `tools/run_integrated_matrix.py` additionally requires a caller-supplied `--reference-matrix` with original artifacts, not only compact summaries.

## Supported scope and time interpretation

The imported model scope is dense Qwen3/llama, one sequence, F16 matrix weights/KV, selected norm/down weight ranges and all-layer paged KV. Other weights and activations remain native. Quantization, MoE, multi-sequence serving and vLLM require separate work.

The baseline uses FCFS Host/NAND, no statistics observer and memory backing. `resident_fragments=256` is a Host budget for logical 64-byte fragments, not GPU frame capacity or Device MOCS. The default four 4KiB frames are a paging-pressure configuration, not a realistic general server KV budget. KV writeback creates a whole-block snapshot; the original Host owns splitting, retries and recovery replay. Completed result/version history can grow with cumulative input.

Real bytes stay in CUDA allocations and capacity backing; HBF models symbolic behavior and timing. The original GPU/host-readiness time mapping is retained. **CPU service and diagnostic overhead can affect future simulated arrivals.** Neither `process_wall_ms` nor final backend time is a predictive token latency. Use the S/F fixed-activity experiments above for the separate modeled token-time construction.

File backing, bounded prefetch, retention/refresh and thermal APIs exist and have separate upstream checks; their simultaneous activation in all model runs is not implied. Refresh requires explicit service calls.

## Distribution and license

Clone/archive the **whole HBF-EventLab branch**, not just this subdirectory. The old standalone `package_source.py create` is disabled because it would omit the current backend; historical archive verification remains available. Do not publish local build identities, profiles, weights or raw traces as source.

Original frontend and overlay contributions are distributed under Apache-2.0 with project-maintainer authorization; existing third-party terms are retained. See [LICENSE](LICENSE), [third-party notices](THIRD_PARTY_NOTICES.md), [dependencies](docs/DEPENDENCIES.md), [architecture](docs/ARCHITECTURE.md) and [validation scope](docs/VALIDATION.md).
