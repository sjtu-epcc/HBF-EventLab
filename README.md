# GPT-2 124M + simulated GPU + incremental HBF source snapshot

This branch is a **source snapshot of the code actually used** for the 2026-11-03 GPT-2/WikiText-2 timing experiment. It is intended for GPU/HBF interface and backend engineers to reproduce and change the slow memory path. It contains no model weights, dataset copy, trace archive, SSH credentials, run logs, or claimed TTFT/TPOT result.

## Provenance

- `EventLab/`: clean archive of `sjtu-epcc/HBF-EventLab` branch `feat/incremental-host` at `8ccb1f353630f893f761b017d621812b7b9d5bf2`.
- `adapter/`, `gpu-headers/`: exact GPU compatibility sources compiled with that upstream commit. `adapter/src/gpu/page_controller.cpp` SHA-256 `b904a60cc9088e6086635b71fb93732a2cbce9c3a45688bb4c80ff88d501459e`.
- `source/hbf_eventlab_bridge.cc`: exact EventLab bridge copied into the GPU simulator. Source hashes and setup details are in `docs/BACKEND_DIAG_REPORT_20261103.md`.
- GPU simulator base: `jager197/accel-sim-hbf` commit `3415a3171d9312fac6a835bc1aa188f7aab685c0`. The tested binary SHA-256 was `4effbff65f59e837f6f488f4fcd9a0e8760d08ca2a1cf7d11fcf4885efc4e7ff`. The simulator repository is not duplicated here.

## Repository layout

| Path | Purpose |
|---|---|
| `EventLab/` | Updated incremental HostDriver, DeviceDriver, runtime, and tests |
| `adapter/src/gpu/page_controller.cpp` | Live GPU sector-to-page admission, merge, and completion handling |
| `gpu-headers/hbf_eventlab/gpu/` | GPU controller declarations used by the adapter |
| `source/CMakeLists.txt` | Builds updated EventLab plus adapter and ten adapter tests |
| `source/hbf_eventlab_bridge.cc` | Accel-Sim memory-request bridge to EventLab |
| `source/run_replay.sh`, `source/collect_metrics.py` | Full-trace replay and strict token-metric collection |
| `source/start_*.sh` | Exact launchers and diagnostic controls used on node2 |
| `capture-source/` | GPT-2 native reference, custom embedding, capture, trace processing, and memory guard scripts |
| `config/gpgpusim.config`, `config/token_boundaries.json` | Exact GPU timing configuration and output-token kernel ranges |
| `docs/` | Measured reason for the four-hour timeout, with actionable code locations |

## Build and reproduce

From the snapshot root on Linux, build the GPU-extended EventLab library and tests:

```sh
cmake -S source -B build-gpu -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build build-gpu -j2
ctest --test-dir build-gpu -j2 --output-on-failure
```

For the full timing replay, obtain the matching HBF-Sim GPU simulator source at the commit above and copy `source/hbf_eventlab_bridge.cc` into its `gpu-simulator/gpgpu-sim/src/gpgpu-sim/`. Build it with `HBF_EVENTLAB_ROOT` set to this snapshot's `EventLab/`, `HBF_EVENTLAB_LIBRARY` set to `build-gpu/backend/libhbf_eventlab.a`, and `CPLUS_INCLUDE_PATH` including `gpu-headers/` and `adapter/third_party/hbf_sim/`. `source/build_simulator.sh` records the exact node2 build procedure but uses node2-specific absolute paths; adapt those paths locally.

The original model weights, WikiText-2 dataset, and 1,480-kernel NVBit replay trace are **external inputs**. The scripts intentionally use the isolated node2 experiment paths and refuse to overwrite old output directories. `config/gpgpusim.config` is copied from the file that the run referenced; update absolute interconnect/trace locations when moving machines. Do not treat the HBM or old HBF diagnostic controls as final model timing results.

## Known issue to investigate

The four-hour direct GPU+EventLab HBF run exited 124 after 29/1480 kernels. In kernel 6, the same trace and instruction count required 30,999,754 GPU cycles on HBF versus 160,036 on an idealized HBM control. Its 1,728 distinct 4KiB pages produced 20,827 page reads and 18,306 NAND senses. The GPU controller merges in-flight reads but erases a page's record at completion (`adapter/src/gpu/page_controller.cpp`, `backend_completed()`); it has no completed-page residency cache. The bridge fixes MSHRs at 32, while this run bypasses GPU L2 for HBF. The intended hardware page-buffer/cache policy must be specified before changing timing semantics. See the report in `docs/` for full counters and limitations.

There is **no valid simulated TTFT/TPOT** from the incomplete run.
