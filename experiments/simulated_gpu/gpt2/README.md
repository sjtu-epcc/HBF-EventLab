# GPT-2 simulated GPU replay

The reference uses GPT-2 124M in FP32, batch 1, an 8-token WikiText-2 prompt and four generated tokens. Accesses to the remapped weights use HBF; activations and KV stay in the simulated GPU memory. The [archived report, input hashes and full configuration](https://github.com/geek-xiaoshuaiji/HBF-EventLab/tree/108d36bceb7187d071e0b428e2d2aaf157fde2bc/results/gpt2-unified-20261109) identify the original run. `reference/per-token.csv` contains its token timings. Model weights, the dataset and full instruction traces are external inputs.

### Inputs

A prepared replay consists of `kernelslist.g`, the trace files it names, `manifest.json` and `image-pages.txt`. To generate these, use `capture_gpt2.py --model MODEL --dataset PARQUET --output DIR` for the native reference; repeat with `--capture --output metadata` in a fresh capture directory under the NVBit tracer (`ACTIVE_FROM_START=0`, `TOOL_COMPRESS=1`). The reference uses the supplied `embedding_cuda.cu` compiled as a shared library and passed via `--embedding-lib` for both executions. Postprocess the captured kernel list using the pinned Accel-Sim NVBit trace postprocessor and place its grouped kernel list and traces in `CAPTURE/traces/`. Run `python3 prepare_replay.py --capture CAPTURE --output REPLAY` to remap weights and write the symbolic initialization image.

### Replay and compare

Build the [simulated GPU adapter](../../../integrations/simulated_gpu/README.md) and source the external simulator's Release environment in the replay shell. The launcher expects a fresh output directory. Example from the repository root:

```sh
exp="$PWD/experiments/simulated_gpu/gpt2"
bin="$SIM_ROOT/gpu-simulator/bin/release/accel-sim.out"
RUN_TIMEOUT=7200 bash "$exp/run_replay.sh" "$bin" "$exp/config/gpgpusim.config" \
  "$SHORT_REPLAY/kernelslist.g" "$REPLAY/image-pages.txt" "$OUT/short"
python3 "$exp/audit_trace.py" "$OUT/short/eventlab.csv"
```

For the published first-two-kernel smoke case, use those two entries from the prepared GPT-2 trace with their referenced files and the **full original image**. Verify two completed kernel UIDs, 768 admitted/completed/returned requests, zero failed/owned and a drained device. Compare cumulative GPU cycles at both boundaries against a run of the same short input on the archived executable. A backend-only test exercises the controller but cannot establish cycle equivalence for the GPU front end.

For the complete case, supply the full kernel list and a suitable timeout (the reference simulation took about 17 hours). The plain lifecycle CSV can be large. The collector accepts either `eventlab.csv` or `eventlab.csv.gz` in the run directory.

```sh
python3 "$exp/collect_metrics.py" --capture "$CAPTURE" --reference "$NATIVE" \
  --replay "$REPLAY" --run "$OUT/full" --output "$OUT/full/summary.json"
```

The reference completed 1,480 kernels in 305,960,191 GPU cycles. Compare per-token cycles and IDs with the archived CSV. The launcher records the input INI, GPU config, MSHR limit, bandwidth override and input hashes. The bridge writes the effective backend INI. The reference uses 512 in-flight 64-byte read keys and overrides GPU channel bandwidth to 188 GB/s to match the device INI.

### Read scheduling observations

Set `HBF_EVENTLAB_READ_OBSERVATIONS` to a fresh absolute output path when launching a short replay. Leave it unset for normal runs. Summarize a completed trace with:

```sh
python3 experiments/simulated_gpu/gpt2/analyze_reads.py "$HBF_EVENTLAB_READ_OBSERVATIONS"
```

Each whitespace-delimited row begins with its event type. The `mapping` rows associate GPU request IDs with backend Global IDs and aligned byte addresses. The merge flag identifies additional waiters on the same in-flight read. The `link` rows connect Global IDs to command attempts. Use `command` for handoff and its hint, `accepted` for formal acceptance, and `enter`, `release` and `service` for the window and cache/sense paths. Device timestamps are simulated picoseconds. A release row includes the reason, batch size and all attempt IDs.

Each `sense-selected` row closes the immediately preceding group of `sense-candidate` rows. Rows identify the attempt and physical channel, die, bank, block and page. A `prior-sense` row records one blocked check, so repeated checks can refer to the same request.

For sequential kernels, `sm` rows contain cumulative GPU cycle, resident kernel UID, SM ID, scoreboard-ready flag, issue flag, and GPU-wide outstanding HBF read count (admission through return). There is one sample per active SM issue cycle, aggregated over its schedulers. A ready instruction can still wait for an execution pipeline. Summing these samples yields **SM-cycles**. A zero-ready sample overlapping an outstanding read indicates correlation; attributing a stall to a particular warp requires consumer tracking. Detailed output grows with active SM-cycles and is intended for short captures.

Response arbitration records use simulated ps, channel and attempt ID. Each `response-read-candidate` identifies an eligible read in the actual link offer. At issue, the analyzer counts the eligible reads on that channel, including decisions won by a write when reads were eligible. The wait is measured from `egress-ready` (the response has acquired egress space) to `response-issue` (link transmission starts). The summary reports positive waits, mean, nearest-rank percentiles and maximum, both overall and by read service. Time spent waiting for an egress slot and the transfer itself are outside this interval.

For a short replay with Host scheduling diagnostics, also set `HBF_EVENTLAB_ORDER_OBSERVATIONS=1`. The CLI emits the same order records under `--trace`. On each successful read handoff, `host-read-option` identifies the transfer attempt, chosen unit, and each eligible read unit with its parent Global and channel-local byte address. A decision snapshot covers the read options offered on that pass. Join to `order-accepted` by transfer attempt to recover Device acceptance order. `order-page` gives the actual physical page at cache or sense service.

`order-wait-begin` records the blocked attempt and one earlier attempt found by the Device check, with both physical page identities. `order-wait-end` closes that subscription on notification or cancellation. Another subscription can begin at the same tick with a different predecessor. Sum durations per blocked attempt when measuring its waiting time. Adding across requests counts overlapping waits separately.

Run the six-read control with `python3 experiments/simulated_gpu/gpt2/host_order_control.py /path/to/hbf_eventlab_cli output-dir`. It uses identical arrivals across policies, validates the device acceptance order and Bank layout, and writes the paired waits and per-case statistics.
