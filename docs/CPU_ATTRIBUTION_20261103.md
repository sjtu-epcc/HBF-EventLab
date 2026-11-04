# GPT-2 updated replay: kernel 6 CPU cost attribution

**Later causal control:** The same sixth-kernel trace and 94,677,930
cumulative instructions needed 30,999,754 simulated cycles with EventLab HBF
but only 160,036 with an ordinary HBM memory model (about 194x). Thus the
GPU core loop's high CPU share is largely a consequence of HBF-induced extra
simulated cycles. See `BACKEND_DIAG_REPORT_20261103.md` for the memory-path
analysis and backend handoff. This section measures CPU work only.

## Question and conclusion

The 120-second smoke ending at kernel 6 is **not evidence of a backend deadlock**. The full replay progressed through 29 completed kernels (last completion cycle 45,227,581) and then exited 124 at its intentional four-hour timeout by 2026-11-03 14:48 CST. It did not complete the 1480-kernel model. The diagnostic run shows substantial cost in both the standard GPU core simulator and the HBF bridge; our GPU/HBF partition loop and trace block dispatch are small contributors. This measures CPU execution time, not modeled GPU TTFT or TPOT.

## Isolated measurement

- Same GPT-2 124M/WikiText-2 8-in/4-out FP32 trace, 121,551 normal image-page writes, HBF weights, HBM activations/KV, same configs and updated `8ccb1f353630f893f761b017d621812b7b9d5bf2` backend.
- Full replay's executable and process were not modified. A copy of the simulator was instrumented and run separately for 120 seconds; it exited 124 by intentional timeout.
- Both original and instrumented 120-second runs completed exactly the same first five kernels, at cycles 5,264 / 54,028 / 81,568 / 87,383 / 118,086, then ran kernel 6.
- Last complete instrumentation snapshot: 1,200,000 GPU simulation loop calls, simulated cycle 579,055; cumulative wall time inside `gpgpu_sim::cycle()` 101.348 s. Timers are nested only where noted and add measurement overhead, so percentages are approximate.

| CPU region | Wall seconds | Share of GPU loop | Meaning |
|---|---:|---:|---|
| GPU core-domain block | 61.992 | 61.2% | Accel-Sim shader core cycle, stats, and block issue; block issue is included below |
| HBF bridge `cycle_at` | 34.545 | 34.1% | EventLab Host/Device advancement |
| GPU/HBF partition `hbf_cycle` calls | 0.553 | 0.5% | Our rotating per-partition integration |
| Other DRAM partition cycles | 2.406 | 2.4% | Standard DRAM scheduling |
| L2 block | 0.761 | 0.8% | Standard L2/partition handling |
| Block issue (inside core) | 1.360 | 1.3% | Includes trace threadblock loading; **not additive** to core |

The remaining ~1.1 seconds are clock selection, interconnect, and other GPU loop work. Earlier uninstrumented 120-second diagnostic found 35.225 seconds inside the HBF bridge after normal preload, consistent with this run's 34.545 seconds. It also found zero `read_wakeups` and `read_rearms`, so the prior broad ResultSpace read-notification regression is absent in this interval. Host history reclamation still visited 1,107,570,187 units, and SenseOrder callbacks numbered 1,349,653; backend optimization room remains.

## Interpretation for ownership

1. The updated HBF backend is functioning and has moved past kernel 6. Its bridge still consumes about one-third of CPU time in this short interval. This does not by itself make the simulated model incorrect.
2. The largest measured cost is ordinary per-cycle GPU core simulation (about 61%). This is in Accel-Sim's core path; the added HBF partition loop is under 1%. Replacing backend code alone cannot remove the measured core-simulation cost.
3. The 120-second window is too short for kernel 6: in the full replay, kernel 6 finished at simulated cycle 31,117,840, versus 118,086 after kernel 5 and only 579,055 reached in the instrumented smoke. Progress through kernel 29 required four hours of CPU time. A full 1480-kernel result is not feasible under the current four-hour timeout.
4. The current data do **not** prove that frontend output or HBF timing semantics are correct; they identify where wall time is spent. No valid simulated TTFT/TPOT is available until all 1480 kernels and lifecycle checks complete.

## Sources

- Server root: `/home/server/hbf-experiments/20261103-gpt2-updated`.
- Original smoke: `smoke-120/run.log`, `smoke-profile.jsonl`; full run: `full-run/run.log`, `full-launcher.rc`.
- Isolated run: `gpu-diag-120/run.log`, `gpu-diag-120/run.rc`, `gpu-diag-launcher.rc`, `gpu-diag-build.log`.
- Instrumentation: `source/instrument_gpu.py`, `source/start_gpu_diag.sh`; instrumented `gpu-sim.cc` SHA-256 `fa0a5c5ba4872c9e9b8b20cf2ab21ff90097121d41499e9ffeb216e9006d612c`.
- Unmodified replay simulator SHA-256 `4effbff65f59e837f6f488f4fcd9a0e8760d08ca2a1cf7d11fcf4885efc4e7ff`; backend library SHA-256 `d2374879974dd422848a4bd0b59be5eb096d71105179eef13ac6d73d4b47a7cf`.
