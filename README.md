# Simulated GPU experiment: unified backend configuration

This experiment snapshot is published on `sjtu-epcc/HBF-EventLab` branch
`exp/simulated-gpu-unified-config`. It preserves the collaborator's experiment
history and directory layout; the backend lives in `EventLab/`. This branch
has independent history from main and is not intended for a direct merge.

Latest changes, local smoke results, limitations, and Linux handoff:
[Unified configuration handoff](docs/GPU_UNIFIED_CONFIGURATION_20261109.md).
Rebuild the Linux GPU simulator and pass a short replay before the full run.
The existing GPT-2 result below was produced before this configuration change.

---

# GPT-2 124M + simulated GPU + incremental HBF source snapshot

## 2026-11-07 full-replay memory repair

The 512-MSHR replay passed the formerly failing kernel 342, then exited 250 after 720/1480 kernels when `std::bad_alloc` hit the launcher's 24 GiB virtual-memory limit. The GPU controller had retained all completed request and result records across kernels. `GpuPageController::retire_completed_history()` now releases those records only after drain, while keeping cumulative statistics; the bridge invokes it at drained kernel statistics boundaries. A two-kernel GPT-2 smoke exited 0, audited 768 GPU requests, and logged two history retirements. Backend/adapter tests passed 525/525. The repaired full replay in `/home/server/hbf-experiments/20261107-gpt2-retired-history/full-run/` completed 1480/1480 kernels with exit 0 and a strict request/token-boundary audit: **simulated TTFT 72.992536 ms, TPOT 48.000169 ms/token**. See [the validated result and evidence](results/gpt2-20261107/README.md). The prior failed outputs remain preserved.

## 2026-11-06 kernel 342 progress and guarded replay

The prior 1480-kernel replay exited 250 inside kernel 342 after completing 341 kernels; it did not reach the first output token. `source/hbf_eventlab_bridge.cc` now accepts `HBF_EVENTLAB_GPU_MSHRS` (default 32, bounded by the simulator outstanding limit). On the exact kernel 342 trace, 512 MSHRs with Accel-Sim deadlock detection enabled completed with `run.rc=0`, 19,186,219 simulated GPU cycles, and 4,824,672/4,824,672/4,824,672 admitted/completed/returned requests (`failed=0`, `owned=0`, Device drained). A complete request lifecycle audit passed. The single-kernel simulator took 1 h 14 min 26 s of wall time; that duration is **not** TTFT.

`source/continue_after_kernel342.sh` validates that single-kernel run before invoking `source/run_replay_mshr512.sh` for the complete GPT-2 trace and `source/collect_metrics.py` for strict TTFT/TPOT extraction. The 1480-kernel run has started on node2 under `/home/server/hbf-experiments/20261106-gpt2-k342-mshr/full-run/`. Its output is not a valid model result until exit 0, all kernels and requests complete, and the token boundaries pass validation. The diagnostic launcher scripts use node2-specific paths; transfer shell scripts with LF line endings.

## 2026-11-05 GPU 接入修正

当前工作分支 `gpt2-64b-multibank-20261105` 在下述 2026-11-03 原始快照上继续开发。GPU sector 读现在按覆盖的 64B 单元提交并完成；本轮 16 channel × 4 die × 16 bank 的临时实验几何结构见 `config/gpt2-eventlab-device.ini`，实际几何在运行日志中打印。多页 block 的初始权重页共享 block 内容版本，保持普通 Host 写入。后端及接入测试 524/524 通过。第 6 个 kernel 由旧接入的 30,999,754 周期降至 4,204,937 周期；完整模拟 TTFT/TPOT 仍待 1480-kernel 回放完成并严格验收。细节见 `docs/GPT2_64B_MULTIBANK_20261105.md`。

以下为 2026-11-03 原始快照的历史记录，旧哈希和旧超时结论只对应原始提交。

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
