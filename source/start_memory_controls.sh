#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261103-gpt2-updated
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
ROOT="$BASE/simulator"
REPLAY="$OLD/replay"
CONFIG="$OLD/gpt2-sim-v5/gpgpusim.config"
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > memory-controls.rc' EXIT
test ! -e "$BASE/control-upstream-120"
export HBF_BACKEND=upstream
export RUN_TIMEOUT=120
export GPGPUSIM_ROOT="$ROOT/gpu-simulator/gpgpu-sim"
export ICXT_CFG="$GPGPUSIM_ROOT/configs/tested-cfgs/SM7_QV100/config_volta_islip.icnt"
export REPO_ROOT="$ROOT" CUDA_INSTALL_PATH=/usr/local/cuda CUOBJDUMP_SIM_FILE=jj
LIBDIR="$(dirname "$(find "$GPGPUSIM_ROOT/lib" -path '*/release/libcudart.so' -print -quit)")"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ulimit -v 25165824
"$ROOT/experiments/common/run_sim.sh" "$BASE/control-upstream-120" "$CONFIG" \
 "$ROOT/gpu-simulator/bin/release/accel-sim.out" -config "$CONFIG" \
 -trace "$REPLAY/kernelslist.g" -gpgpu_hbf_channel_bw_gbps 188 \
 -gpgpu_hbf_trace_file "$BASE/control-upstream-120/hbf.csv"
