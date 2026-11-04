#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261103-gpt2-updated
ROOT="$BASE/simulator-gpu-diag"
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > gpu-diag-launcher.rc' EXIT
test ! -e "$ROOT"
cp -a --reflink=auto "$BASE/simulator" "$ROOT"
python3 "$BASE/source/instrument_gpu.py" "$ROOT"
export HBF_EVENTLAB_ROOT="$BASE/EventLab"
export HBF_EVENTLAB_LIBRARY="$BASE/build-gpu/backend/libhbf_eventlab.a"
export CPLUS_INCLUDE_PATH="$BASE/gpu-headers:$BASE/adapter/third_party/hbf_sim"
export CUDA_INSTALL_PATH=/usr/local/cuda
export PATH="$CUDA_INSTALL_PATH/bin:$ROOT/.local-deps/root/usr/bin:$PATH"
set +u
source "$ROOT/gpu-simulator/setup_environment.sh" release
set -u
make -j2 -C "$ROOT/gpu-simulator" > "$BASE/gpu-diag-build.log" 2>&1
sha256sum "$ROOT/gpu-simulator/bin/release/accel-sim.out" > "$BASE/gpu-diag-bin.sha256"
sed "s@ROOT=\"\$BASE/simulator\"@ROOT=\"\$BASE/simulator-gpu-diag\"@" "$BASE/source/run_replay.sh" > "$BASE/source/run_gpu_diag_replay.sh"
chmod +x "$BASE/source/run_gpu_diag_replay.sh"
unset HBF_DIAG_OUTPUT
bash "$BASE/source/run_gpu_diag_replay.sh" "$BASE/gpu-diag-120" 120 > "$BASE/gpu-diag-run.log" 2>&1
