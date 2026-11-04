#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261103-gpt2-updated
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > gpu-validation.rc' EXIT
test ! -e adapter
mkdir -p adapter/src adapter/tests
# Only GPU headers are exposed. Do not shadow updated upstream headers.
mkdir -p gpu-headers/hbf_eventlab
cp -a "$OLD/EventLab/include/hbf_eventlab/gpu" gpu-headers/hbf_eventlab/
cp -a "$OLD/EventLab/src/gpu" adapter/src/
cp -a "$OLD/EventLab/third_party" adapter/
cp "$OLD/EventLab/tests/gpu_incremental.cpp" adapter/tests/
cp "$OLD/source/probe_preload.cpp" source/
cmake -S source -B build-gpu -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON > gpu-configure.log 2>&1
cmake --build build-gpu -j2 > gpu-build.log 2>&1
ctest --test-dir build-gpu -j2 --output-on-failure > gpu-tests.log 2>&1
./build-gpu/probe_current 4096 probe-32.json 16 burst4096 32 > probe-32.log 2>&1
