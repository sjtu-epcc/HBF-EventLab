#!/usr/bin/env bash
# Build inside a dedicated HBF-Sim checkout prepared with upstream make bootstrap.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
sim=$(realpath "${1:?usage: build.sh HBF_SIM_CHECKOUT BUILD_DIR}")
build=$(realpath -m "${2:?provide an EventLab build directory}")
gpu="$sim/gpu-simulator/gpgpu-sim"
[[ $(git -C "$sim" rev-parse HEAD) == 3415a3171d9312fac6a835bc1aa188f7aab685c0 ]] || { echo 'Unexpected HBF-Sim revision' >&2; exit 1; }
[[ $(git -C "$gpu" rev-parse HEAD) == 6c3cf4ff32110908386d605a7034fc67666a92de ]] || { echo 'Run upstream make bootstrap at the pinned revision first' >&2; exit 1; }
patch="$here/eventlab_backend.patch"
if ! git -C "$gpu" apply --reverse --check "$patch" 2>/dev/null; then
    git -C "$gpu" apply --check "$patch"
    git -C "$gpu" apply "$patch"
fi
cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DHBF_EVENTLAB_SIMULATED_GPU=ON
cmake --build "$build" --target hbf_eventlab_simulated_gpu --parallel "${JOBS:-2}"
install -m 644 "$here"/hbf_eventlab_bridge.{cc,h} "$gpu/src/gpgpu-sim/"
export HBF_EVENTLAB_ROOT="$repo"
export HBF_EVENTLAB_LIBRARY="$build/integrations/simulated_gpu/libhbf_eventlab_simulated_gpu.a $build/libhbf_eventlab.a"
export CUDA_INSTALL_PATH="${CUDA_INSTALL_PATH:-/usr/local/cuda}"
export PATH="$CUDA_INSTALL_PATH/bin:$PATH"
set +u
source "$sim/gpu-simulator/setup_environment.sh" release
set -u
make -C "$sim/gpu-simulator" -j"${JOBS:-2}"
