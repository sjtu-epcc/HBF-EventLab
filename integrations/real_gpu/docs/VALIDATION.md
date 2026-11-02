# Upstream release validation (historical)

**All results below describe the imported RealGPUAcess_v0 snapshot, not a new
run against this repository's backend or current machine.** The 716-case suite
combines CPU, CUDA and injection checks. Current integration evidence and missing
model validation are recorded separately in the [integration guide](../../../docs/gpu_integration.md).

Final acceptance is recorded in [release-validation.json](validation/release-validation.json).
Release checks use binaries built in this release directory.

| Evidence | Scope |
|---|---|
| core-tests.xml / core-tests.log | Fresh full CPU/CUDA/injection regression; 716 passed, zero failures/skips |
| operators.json | Fresh 18-case independent attention/PTX/rejection suite |
| validator-tests.log | Fresh five-test independent Python validation contract |
| model-validation.json | Fresh Qwen3-0.6B short/32 and SmolLM2-135M long/64, all three modes |
| candidate-matrix.json | Previously accepted nine-case, 27-process candidate matrix; inherited provenance, not another release-directory run |
| backend-provenance.json | Complete pinned backend identity and six local patch files |
| environment-adaptation.json | Later environment configuration and platform-adapter checks |

Each fresh model run checks complete logits, prompt/generated tokens, exact
selected byte/sector coverage, actual GPU waits/kernels, KV publication, eviction/
writeback, final Host fragment outcomes, resident budget, closed input and resource
drain. Complete logits and tokens are compared with corresponding accepted
candidate outputs and previous main-backend references.

The 18-case suite includes dimensions of the larger Qwen3-1.7B model. The release
checks ran two representative model cases. The nine-case candidate matrix is
earlier validation evidence, not a new run of all nine cases.

Validation includes same-Tick continuation, future watermarks, finite Host input
budgets, physical retry/replay, logical block snapshot ordering and normal-failure
drain. Partial initialization is not reported as application writeback. Severe
Session errors or exhausted run limits may prevent drain and cannot count as
successful accesses.

The validated environment is Windows x64, Visual Studio 2022, CUDA 13.0, RTX 5070
SM120a and Python 3.12. Large raw traces and build products are not distributed
in the source package. Model weights and toolchains are external inputs.

Model acceptance uses FCFS, no attached statistics sink and memory backing.
File, prefetch, thermal and refresh interfaces have independent core checks.
Their simultaneous activation in every model run is not implied. Existing GPU
timestamp time mapping is retained; wall time is software cost, not a prediction
of hardware throughput or a measured-compute TTFT/TPOT experiment.
