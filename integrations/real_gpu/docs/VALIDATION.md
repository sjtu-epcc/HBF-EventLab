# GPU validation scope

Build commands are in the [frontend README](../README.md). Run CTest for the
chosen CPU/CUDA configuration and the standalone Python environment, identity
and numerical-validator tests. The default backend suite does not require a GPU.

## Fixed-activity S/F experiments

[S](../experiments/gpu_s/README.md) checks selected dependencies, numerical
reference comparisons and token-time conservation. [F](../experiments/gpu_ffn/README.md)
checks FFN coverage, native/joint logits and tokens, fragment outcomes, parent
completion, final drain and conservation. Each execution records its effective
configuration, build identity and audit results. F2/F3 reuse a verified F1
activity table. One token does not define TPOT; use multiple outputs for TPOT.

## Asynchronous and paged-KV runtime

Run the frontend CPU/CUDA and model-specific checks separately. The
`lab_shared_gpu_owner` and `lab_shared_gpu_scaled` overlap-coverage checks have
shown scheduling-sensitive failures; passing isolated runs does not resolve
that limitation. S/F acceptance does not establish asynchronous overlap coverage.

Model execution and process injection require their own explicit checks.
Software wall time, simulated access latency and modeled token times use
separate definitions. HBF timing parameters are uncalibrated model inputs.

## Result provenance

[Published S/F results](gpu-s-f-new-backend/README.md) specify their original
hardware, backend revision and experiment settings. Compact upstream records
under `validation/` are historical results, not evidence of running a different
checkout. Reproduction requires the matching external model and raw inputs;
local paths, build outputs and new run artifacts should remain outside tracked documentation.
