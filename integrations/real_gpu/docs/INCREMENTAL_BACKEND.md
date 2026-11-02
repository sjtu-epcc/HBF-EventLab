# Incremental backend adaptation

> Historical upstream adaptation record: the pinned commit, six-file overlay,
> and test/performance results below describe the imported snapshot only.
> This integration builds the current HBF-EventLab repository root instead;
> do not reapply the historical overlay. See the [current integration guide](../../../docs/gpu_integration.md).

This release pins HBF-EventLab upstream commit
`72e074508c462dceb7d42962c4885ecf17cc4d0d`. The complete upstream archive has
205 files. A two-file local Session extension permits guarded installation of a
program-outcome fault function before any input or advancement. Original and
patched hashes are recorded in `third_party/hbf-eventlab-origin.json`.
Four additional Host files reduce historical scans without changing event
decisions: before the first failed Write response, recovery/retry/abort scans are
dormant; reclamation visits only versions whose units were retired. Once a Write
error occurs, the original error-history scans run for the rest of the Session.
All upstream history and sealed-version checks remain intact. The combined local
overlay therefore changes six upstream files; it is not an unmodified checkout.
Ungrouped Regular reads also skip a predecessor scan whose grouping/Batch
conditions cannot apply. The normal/fault trace comparison and all 466 upstream
tests were repeated after this addition.

The isolated optimization passed 466 upstream tests, 12 byte-exact normal/fault
CLI trace comparisons, and deterministic benchmark comparisons. In the dedicated
2,048-block history fixture, baseline software wall time divided by optimized
wall time was approximately 8.24, measured on the history-scan variant before the
Regular-read shortcut. This is simulator software cost, not modeled
HBF hardware performance. Raw before/after results and the interrupted slow model
diagnostic are retained in the development validation archive; the release
contains compact acceptance and backend provenance records.

## Execution path

```text
CUDA kernels / selected llama.cpp weight accesses / paged KV
    -> Runtime or AsyncRuntime (one LiveTimingService owner thread)
    -> TimingBackend producer adapter
    -> SimulationSession
    -> original HostDriver
    -> original DeviceDriver
    -> final Global result -> parent access complete -> dependent GPU work
```

The upstream synchronous TensorAccessBridge remains unchanged. Its immutable
weight initialization, fixed-gap and zero-gap reads are tested against the
producer adapter on the same configuration. The shared asynchronous model path
uses SimulationSession directly through TimingBackend. It does not invoke the
synchronous immutable TensorAccessBridge for every GPU request: that interface
owns its Session and does not support mutable KV or concurrent parents. This is
the shared-session adaptation needed to preserve mutable KV, concurrent parents,
and the existing weight/KV clock and asynchronous frontend.

The adapter keeps parent IDs, byte-range coverage and logical snapshot ordering.
It submits only GlobalRequest values. Device commands, sending policy, physical
program order, command retries and recovery replay belong to the original Host.
There is no frontend Device submission or extra command-credit window.

## Immutable reads and mutable logical snapshots

Frontend byte requests are rounded to the exact union of touched 64-byte sectors.
Adjacent sectors in one DLU are grouped into one Global, without covering holes.
Each Global read carries the current expected block content identity; the Host
checks its final logical result. Parent completion is the maximum final child
completion time. Acceptance or a single command response does not complete it.

Reservations initialize whole DLUs using ordinary Global writes. All pages of a
logical block initially share content 1. A page writeback creates a new immutable
identity for the entire logical block and submits its initialized pages as a
logical snapshot, starting at page 0. It does not create per-page versions.
Logical RAW/WAR/WAW ordering excludes older conflicting parents until the
snapshot completes. Other logical blocks can progress concurrently.

Real bytes and page hashes remain in CapacityStore/GPU allocations. Updating a
symbolic block identity changes neither unchanged real pages nor immutable
weights, even when weight and KV reservations share a logical block. Independent
capacity, file, transaction and numerical tests check the real data separately.
DataToken is symbolic evidence, not a payload hash or byte-transfer mechanism.

The compatibility statistic `replayed_pages` counts normal logical snapshot
pages written by the frontend. It does not count Host recovery replay commands;
the latter are exported separately as `host_replay_commands`.

## Budgets and time

| Setting | Release meaning |
|---|---|
| `resident_fragments=256` | Maximum active Host logical 64-byte fragments; minimum 64 for full-DLU initialization |
| Parent limit | Bounds frontend outstanding parents, including unconsumed results |
| Global grouping | Contiguous sectors within a DLU; initialization/snapshot writes cover a whole DLU |
| `outstanding=256` | Device-selected MOCS; independent of the Host resident budget |
| Host writes | Explicit Burst4096, using the device's advertised burst capability |
| Host read hints | AllRegular, matching the existing direct-device read behavior |
| Host/NAND policies | FCFS / FCFS |
| Statistics observer | None in the acceptance model baseline |
| Host lifetime guard | Explicitly 1,000,000,000 logical fragments; active storage remains bounded separately |
| Event guard | Existing per-operation/per-stream frontend event budget, converted to an absolute Session ceiling |
| Simulated-time guard | No finite horizon, matching the existing GPU-timestamp-driven frontend; event guard remains active |

Capacity rejection retains the producer range. Reclamation notifications trigger
rechecking the budget. `backpressure` now counts producer/Host-input deferrals;
it is not the old direct-device NotTaken counter. Pressure tests deliberately
exceed the active Host budget; the asynchronous GPU fixture uses a declared 1 ms
sense latency to create overlap and make rejection reproducible.
The active-fragment limit does not bound total Host RAM: completed Global results
and content-version history remain available for duplicate-ID, dependency and
sealed-version checks. Their storage can grow with cumulative input.

Finite advancement respects exclusive watermarks. Future producer arrivals are
not registered early. A completion at the current Tick resumes through the
Session's causal feedback continuation, without adding a dummy Tick. Compute-only
empty contributions advance time without creating dummy memory traffic.
Observed GPU readiness timestamps retain the existing frontend time-mapping
policy; this migration does not introduce the upstream Qwen executor's fixed-gap
or measured-compute-record experiment as a replacement.

`end_stream()` checks temporary drain while permitting a subsequent stream.
`finish()` closes external input permanently, checks workload success and verifies
Host/Device drain. Asynchronous finalization calls both in order. A normal final
logical failure abandons the unregistered producer suffix, closes external input
and drains all registered Globals before poisoning the frontend. It is not
misclassified as an implementation exception inside a Session callback. Severe
Session errors or event-limit exhaustion retain diagnostics and may prevent a
complete drain. Every failed path prevents dependent execution. Initialization
programs are counted separately from application writebacks, even when only an
initial prefix succeeds.

## Validation and source completeness

See [release validation](VALIDATION.md), the compact records under validation/,
and [source provenance](../third_party/hbf-eventlab-origin.json). Fresh release
checks are distinguished from the inherited nine-case candidate matrix.

Source packaging preserves nested build-* filenames such as
cmake/build-info.cmake. It excludes root build directories, model inputs, local
profiles and large traces. The source archive and relative release-manifest.json
are generated and verified by ../tools/package_source.py. Rebuild after moving
or extracting the source tree to create a new location-bound build identity.
