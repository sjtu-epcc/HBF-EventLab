# Incremental integration review

Date: 2026-10-31. Scope: current implementation, incremental backend contract and
the GPU experiment handoff. This review did not implement or run the proposed
inference/TTFT/TPOT experiment.

## Conclusion

No new blocking defect was found in the reviewed incremental integration. The
current shared asynchronous producer path satisfies the documented release
contract. It does not yet satisfy the complete experimental deliverables in the
GPU handoff. Passing integration tests must not be described as completing that
experiment.

## Verified integration

| Requirement | Evidence and result |
|---|---|
| Original Host and Device | src/backend.cpp constructs SimulationSession and submits GlobalRequest values through try_submit/try_submit_feedback. No frontend Device command submission or child_window was found in the reviewed active path. |
| Initialization | reserve submits whole-DLU ordinary Global writes and waits for final outcomes. Partial initialization poisons further use and is not counted as application writeback. |
| Final-result dependency | Parent pending count is decremented from GlobalCompletion. Non-Success triggers producer abandonment and registered-work drain; dependent GPU loads check terminal/completion status before reading real bytes. |
| Backpressure | Producer retains the unregistered range and retries after capacity/completion feedback. resident_fragments is an active logical 64B fragment budget, distinct from MOCS and GPU frames. |
| Content identity | Immutable reservations start with block content 1. Updates construct a new whole-block logical snapshot, preserve unchanged pages, and order conflicting RAW/WAR/WAW parents. Physical splitting/retry/recovery remains with Host. |
| Timing causality | Future arrivals are not registered early. Exclusive watermarks and Session feedback continuations handle same-Tick progress. Empty compute contributions create no dummy traffic or dummy Tick. |
| Shutdown | AsyncRuntime ends its stream and calls backend.finish; finish closes input and checks workload success plus Host/Device drain. end_stream alone is temporary drain. |
| Failure/limits | Normal final logical failure stops unregistered input and drains registered Globals. Severe errors/limits poison the frontend and do not claim successful completion or guaranteed drain. |
| Byte authority | GPU/Capacity retain actual bytes; DataToken and content identity are symbolic. No physical NAND payload-transfer claim is supported. |

The immutable synchronous TensorAccessBridge is a tested reference. The current
model path uses the shared Session producer through TimingBackend, rather than
calling TensorAccessBridge for each access. This is the explicit adaptation in
docs/INCREMENTAL_BACKEND.md for concurrent immutable weights and mutable KV.
It is not literally the per-access bridge path drawn in the original handoff.
The proposed synchronous experiment must use the bridge as specified in its own
design; it has not been implemented merely by validating the shared path.

The legacy synchronous Runtime::serve is reusable and does not itself perform a
permanent backend.finish. Its drained() is a temporary drain check. The verified
model/LiveTimingService finalization above does perform permanent close. Results
from these APIs must not conflate the two lifecycle meanings.

## Experimental requirements still pending

- Measured GPU compute segments, frozen matching tables and G2/G3 replay.
- Explicit request/prefill/token semantic events and per-token modeled timestamps.
- General multi-region manifests with source offsets, alias/padding and independent
  registered/touched/cumulative coverage accounting.
- Independent CPU/native/HBF whole-model multi-token numerical comparison.
- Separate verification/timing modes without diagnostic snapshots in timed work.
- Normal EOS and N=0/1 handling: current model runner requires 2..448 outputs and
  treats early EOG as an error for fixed-length acceptance.
- Modeled TTFT/TPOT/ITL, per-token conservation and the three requested plots.

Current process_wall_ms ends before sampling and is not token delivery latency.
The online GPU-timestamp policy also correlates CPU media readiness through a
wall-clock/GPU epoch. It is the existing online policy, not a pure compute-table
clock that excludes simulator software cost. It cannot serve as the modeled
fixed-compute baseline without implementing the planned experiment mode.

## Fresh checks

- Compared 205 backend files and six producer/runtime/order files with the release
  manifest: 211/211 matched. Checked all six declared overlay hashes: matched.
- Fresh CPU build and focused regression: 38/38 passed, zero failures.
- Fresh shared-session CUDA build and regression: 8/8 passed, zero failures.
- The CUDA checks cover mixed access, pressure, owner checks, failure, abort,
  frame guard, scaled time and KV.

Local records: build-audit-logs/source-check.json, configure.log, build.log,
tests.log, results.xml, gpu-configure.log, gpu-build.log, gpu-tests.log and
gpu-results.xml. Binaries were built in build-audit-incremental from the current
source. MSVC compilation ran outside the sandbox because compiler probing had
previously encountered sandbox-related program-database failures.

This is a focused integration review, not a fresh full-backend regression,
whole-model numerical rerun, multi-channel validation or hardware calibration.
No implementation files were changed by this review.
