#!/usr/bin/env bash
# Run only after rebuilding Accel-Sim with this bridge/header/library.
set -euo pipefail
if (( $# < 5 || $# > 6 )); then
  echo "usage: $0 accel-sim.out gpu.config kernelslist.g image-pages.txt NEW_RUN_DIR [backend.ini]" >&2
  exit 2
fi
BIN=$(realpath "$1")
GPU_CONFIG=$(realpath "$2")
TRACES=$(realpath "$3")
IMAGE=$(realpath "$4")
HERE=$(cd "$(dirname "$0")" && pwd)
INI=$(realpath "${6:-$HERE/../config/gpt2-eventlab-device.ini}")
test ! -e "$5"
mkdir -p "$5"
RUN=$(realpath "$5")
cp "$INI" "$RUN/backend-input.ini"
cp "$GPU_CONFIG" "$RUN/gpgpusim.config"
export HBF_BACKEND=eventlab HBF_EVENTLAB_DEVICE_CONFIG="$RUN/backend-input.ini"
export HBF_EVENTLAB_IMAGE_PAGE_FILE="$IMAGE" HBF_EVENTLAB_PRELOAD_IMAGE=1 HBF_EVENTLAB_SEED_IMAGE=0
export HBF_EVENTLAB_GPU_MSHRS="${HBF_EVENTLAB_GPU_MSHRS:-512}" HBF_EVENTLAB_TRACE="$RUN/eventlab.csv"
unset HBF_EVENTLAB_PAGES_PER_CHANNEL HBF_EVENTLAB_UCIE_GRADE HBF_EVENTLAB_BURST_WRITES
unset HBF_EVENTLAB_RESIDENT_FRAGMENTS HBF_EVENTLAB_FAIL_ONCE
# Source the GPU simulator's environment before invoking this script.
# Keep a short timeout by default. A complete run is a separate explicit action.
set +e
timeout "${RUN_TIMEOUT:-120}" "$BIN" -config "$GPU_CONFIG" -trace "$TRACES" \
  -gpgpu_hbf_channel_bw_gbps "${GPU_CHANNEL_BW_GBPS:-188}" \
  -gpgpu_hbf_trace_file "$RUN/hbf.csv" -gpgpu_deadlock_detect 1 >"$RUN/run.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" >"$RUN/run.rc"
exit "$rc"
