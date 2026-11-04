#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261103-gpt2-updated
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
ROOT="$BASE/simulator"
cd "$BASE"
test "$(cat gpu-validation.rc)" = 0
test ! -e "$ROOT"
cp -a --reflink=auto "$OLD/HBF-Sim-eventlab" "$ROOT"
cp source/hbf_eventlab_bridge.cc "$ROOT/gpu-simulator/gpgpu-sim/src/gpgpu-sim/"
export HBF_EVENTLAB_ROOT="$BASE/EventLab"
export HBF_EVENTLAB_LIBRARY="$BASE/build-gpu/backend/libhbf_eventlab.a"
# Makefiles expect GPU headers beneath HBF_EVENTLAB_ROOT/include, so add a
# separate include path without installing old headers over updated upstream.
export CPLUS_INCLUDE_PATH="$BASE/gpu-headers:$BASE/adapter/third_party/hbf_sim"
export CUDA_INSTALL_PATH=/usr/local/cuda
export PATH="$CUDA_INSTALL_PATH/bin:$ROOT/.local-deps/root/usr/bin:$PATH"
set +u
source "$ROOT/gpu-simulator/setup_environment.sh" release
set -u
make -j2 -C "$ROOT/gpu-simulator"
sha256sum "$ROOT/gpu-simulator/bin/release/accel-sim.out" "$HBF_EVENTLAB_LIBRARY"
