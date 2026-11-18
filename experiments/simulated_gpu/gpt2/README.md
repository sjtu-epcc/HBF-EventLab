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
