# Full FFN expansion: F1, F2 and F3

F1 executes Qwen3-0.6B on the GPU, with its original F16 KV cache, and gates
the complete layer-10 FFN gate/up/down matrices through HBF-EventLab. The three
F16 matrices contain 18 MiB. A stable 84-matrix layout reserves their addresses
at 180, 186 and 192 MiB in a 512 MiB logical device; only the selected matrices
are registered and initialized.

F2 selects layers 0, 10, 20 and 27 (12 matrices, 72 MiB) at their original
full-layout addresses. It requires a passed reference batch and verifies the
model, prompt, native settings, source/binary/DLL identities, HBF configuration,
artifact hashes and reconstruction of the compute table from raw GPU activity.
The verified native records are copied into the new batch; no second native
GPU run is performed. The CUDA executable and HBF parameters stay identical.

F3 selects all 28 layers (84 matrices, 504 MiB), using the same reference
validation and unchanged 512 MiB configuration. Eight inference steps require
672 parent reads and 4032 MiB of cumulative reads. Based on F2, allow about
3 hours 15 minutes in the upstream run; measure software cost again on the integrated build. The command below sets a 6-hour
process timeout to allow variation. This wall-time estimate is not HBF latency.

The entry point compiles the S8 CUDA executor with separate FFN gate/segment
headers. Existing S8 builds retain their original headers and behavior. The
backend and llama runtime are built from this workspace, in a separate directory.
No sealed or historical build is overwritten.

## Build and run

From `integrations/real_gpu`, with Python 3.11+/NumPy, Visual Studio 2022 C++ tools and
CUDA/CUPTI installed:

```powershell
./experiments/gpu_ffn/build.ps1 -CudaRoot <CUDA-directory> -BaseBuild ./build-sf-model-20261105 -BuildDir ./build-ffn
python -m unittest discover -s experiments/gpu_ffn -p test_run.py -v
python experiments/gpu_ffn/run.py --model <Qwen3-0.6B-F16.gguf> --cuda-root <CUDA-directory> --output output/gpu-ffn/f1-001
python experiments/gpu_ffn/run.py --stage F2 --reference output/gpu-ffn/f1-001 --model <Qwen3-0.6B-F16.gguf> --cuda-root <CUDA-directory> --output output/gpu-ffn/f2-001
python experiments/gpu_ffn/run.py --stage F3 --reference output/gpu-ffn/f1-001 --model <Qwen3-0.6B-F16.gguf> --cuda-root <CUDA-directory> --output output/gpu-ffn/f3-001 --timeout 21600
```

The GPU target is selected when building the shared model dependencies;
use this frontend's `build.ps1 -CudaArchitectures` option there. Every run requires a new output directory. Use
`--prepare-only` to inspect the actual GGUF layout, model hash, selected regions,
H0 configuration and expected counts without running GPU inference. `--timeout`
sets the wall-time budget per subprocess (default 3600 seconds). Interrupted or
failed outputs are retained and must not be reused as a fresh output directory.

The default prompt is the original S-series input. One native request captures
kernel/memcpy/memset activity in synchronized windows and exports complete logits.
The runner freezes the per-window GPU interval unions into a compute table. One
F1 request consumes that exact table while performing real GPU work and HBF
accesses. Both use one-node dispatch, F32 matrix accumulation and native F16 KV;
no intermediate-array snapshots are written.

## Lightweight backend audit

`OnlineAudit` independently checks the manifest's expected address intervals,
ordered Global/fragment identities, exact 4 KiB/64 B coverage, terminal outcomes,
and parent completion at the maximum child completion time. It discards completed
fragment and attempt entries. Observer memory therefore follows live work rather
than all cumulative accesses. Only parent-level gate results are exported.

The Host still retains its own completed Global/version history. Peak process
working set is measured on Windows; bounded observer maps do not imply constant
total process memory. CUPTI kernel/API/window records remain available for the
selected-consumer launch-order audit. These are small GPU records, not millions
of backend fragment rows.

The runner checks native/F1 consumer sequences, complete logits and tokens,
actual per-step FFN coverage, ordinary initialization, online fragment totals,
Host success, close/drain, every compute-table entry, selected CUDA launch API
ordering, and integer time conservation. With eight inference steps the expected
counts are 24 parent reads, 36,864 read Globals, 2,359,296 read fragments and
294,912 initialization fragments. EOS is retained separately; useful-token
metrics and expectations use the actual executed steps.

For F2 at eight steps, the expected counts are 96 parent reads, 147,456 read
Globals, 9,437,184 read fragments and 1,179,648 initialization fragments. The
same independent auditor selects the stage from the saved plan. F3 at eight
steps requires 672 parent reads, 1,032,192 read Globals, 66,060,288 read fragments
and 8,257,536 initialization fragments. Recheck any
stage with `python experiments/gpu_ffn/validate.py --output <batch> --negative-checks`.

## Outputs and interpretation

- `layout-all-ffn.json`, `regions.json`, `plan.json`, prompt and full effective INI.
- `build-identity.json`: model-run binary, runtime DLL, build cache and source hashes.
- `native/` and `F1/`, `F2/` or `F3/`: execution/segment/API/kernel records, full logits and summaries.
- The stage's `gates.jsonl`, `tokens.jsonl`, `session.json`: parent accesses, modeled token
  times, bounded-audit counters and backend final state.
- `compute-table.json`, `paired-tokens.csv`, `result.json`, process commands/logs
  and an artifact hash index. Errors produce `runner-failure.json`.

TTFT/TPOT and waits in `result.json` use modeled picoseconds. Whole-process wall
time, backend initialization/read-call/finish time, and peak working set are
software costs reported separately. A single native activity trace supports a
fixed-trace mechanism comparison, not a timing distribution or hardware prediction.
Real weights remain in GPU storage; this experiment does not implement physical
weight offload. F2/F3 save `reference-binding.json`
and reports only its newly executed subprocess cost; the reused native process
cost remains in the reference batch and is not presented as a new measurement.

## Main-repository integration

See [integration guide](../../../../docs/gpu_sf_integration.md). `BASE_BUILD`
must be built from this checkout's integrated llama entry and sealed using
`tools/integrated_build_identity.py`. The F build compiles the current repository
backend itself; no backend vendor tree or overlay is used. The build wrapper
runs tests and seals `experiment-identity.json`; runs verify that seal before and
after inference. F2/F3 also require the same experiment identity as their F1 reference.
Use a fresh build directory after changing compiled inputs. Optional `-CuptiRoot`
and `-Python` select a standalone CUPTI SDK and Python environment.

CPU-only checks (from the repository root):

```sh
cmake -S integrations/real_gpu/experiments/gpu_ffn -B build-ffn-cpu -DGPU_FFN_BUILD_GPU=OFF
cmake --build build-ffn-cpu --config Release
ctest --test-dir build-ffn-cpu -C Release --output-on-failure
```

This includes the bounded auditor, Python contracts, and a synthetic gate test
using the actual main-repository Host/Device, including initialization failure.
It does not require CUDA or model weights.
