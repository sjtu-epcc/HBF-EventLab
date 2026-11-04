#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261028-gpt2-incremental
TRACER=/tmp/hbf-complete-20261022/HBF-Sim-eventlab/util/tracer_nvbit/tracer_tool/tracer_tool.so
PYTHON=/home/server/anaconda3/envs/gpt2_kv/bin/python
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
export PATH=/usr/local/cuda/bin:$PATH
mkdir -p "$BASE/probe-plain" "$BASE/probe-transformers"
for mode in plain transformers; do
  cd "$BASE/probe-$mode"
  flags=()
  if [[ $mode == transformers ]]; then flags+=(--transformers); fi
  set +e
  timeout 60 env LD_PRELOAD="$TRACER" ACTIVE_FROM_START=0 TOOL_COMPRESS=1 \
    "$PYTHON" "$BASE/source/probe_trace.py" "${flags[@]}" > console.log 2>&1
  rc=$?
  set -e
  printf '%s\n' "$rc" > exit_code
  printf '%s rc=%s\n' "$mode" "$rc"
  tail -n 6 console.log
done
