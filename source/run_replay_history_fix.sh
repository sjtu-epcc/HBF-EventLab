#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261107-gpt2-retired-history
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
ROOT="$BASE/simulator"
RUN="${1:?new output directory required}"
export RUN_TIMEOUT="${2:-172800}"
TRACE_LIST="${3:-$OLD/replay/kernelslist.g}"
CONFIG="$OLD/gpt2-sim-v5/gpgpusim.config"
export HBF_BACKEND=eventlab HBF_EVENTLAB_PRELOAD_IMAGE=1 HBF_EVENTLAB_SEED_IMAGE=0
export HBF_EVENTLAB_IMAGE_PAGE_FILE="$OLD/replay/image-pages.txt"
export HBF_EVENTLAB_PAGES_PER_CHANNEL=8192 HBF_EVENTLAB_UCIE_GRADE=3
export HBF_EVENTLAB_DEVICE_CONFIG="$BASE/config/gpt2-eventlab-device.ini"
export HBF_EVENTLAB_RESIDENT_FRAGMENTS=4096 HBF_EVENTLAB_BURST_WRITES=1
export HBF_EVENTLAB_GPU_MSHRS=512 HBF_EVENTLAB_TRACE="$RUN/eventlab.csv"
export GPGPUSIM_ROOT="$ROOT/gpu-simulator/gpgpu-sim"
export ICXT_CFG="$GPGPUSIM_ROOT/configs/tested-cfgs/SM7_QV100/config_volta_islip.icnt"
export REPO_ROOT="$ROOT" CUDA_INSTALL_PATH=/usr/local/cuda CUOBJDUMP_SIM_FILE=jj
LIBDIR="$(dirname "$(find "$GPGPUSIM_ROOT/lib" -path '*/release/libcudart.so' -print -quit)")"
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ulimit -v 25165824
"$ROOT/experiments/common/run_sim.sh" "$RUN" "$CONFIG" \
 "$ROOT/gpu-simulator/bin/release/accel-sim.out" -config "$CONFIG" \
 -trace "$TRACE_LIST" -gpgpu_hbf_channel_bw_gbps 188 \
 -gpgpu_hbf_trace_file "$RUN/hbf.csv" -gpgpu_deadlock_detect 1
