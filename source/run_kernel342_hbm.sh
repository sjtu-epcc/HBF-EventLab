#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261105-gpt2-64b-multibank
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
ROOT="$BASE/simulator"
REPLAY="$BASE/kernel342-trace"
RUN="${1:?new output directory required}"
export RUN_TIMEOUT="${2:-120}"
CONFIG="$OLD/gpt2-sim-v5/gpgpusim.config"
export GPGPUSIM_ROOT="$ROOT/gpu-simulator/gpgpu-sim"
export ICXT_CFG="$GPGPUSIM_ROOT/configs/tested-cfgs/SM7_QV100/config_volta_islip.icnt"
export REPO_ROOT="$ROOT" CUDA_INSTALL_PATH=/usr/local/cuda CUOBJDUMP_SIM_FILE=jj
LIBDIR="$(dirname "$(find "$GPGPUSIM_ROOT/lib" -path '*/release/libcudart.so' -print -quit)")"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ulimit -v 25165824
"$ROOT/experiments/common/run_sim.sh" "$RUN" "$CONFIG" \
 "$ROOT/gpu-simulator/bin/release/accel-sim.out" -config "$CONFIG" \
 -trace "$REPLAY/kernelslist.g" -gpgpu_hbf_enabled 0 \
 -gpgpu_hbf_trace_file "$RUN/hbf.csv"
