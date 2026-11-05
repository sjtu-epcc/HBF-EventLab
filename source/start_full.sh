#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261105-gpt2-64b-multibank
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > full-launcher.rc' EXIT
grep -q '100% tests passed, 0 tests failed out of 524' ctest.log
test ! -e full-run
unset HBF_DIAG_OUTPUT
sha256sum source/hbf_eventlab_bridge.cc adapter/src/gpu/page_controller.cpp \
 config/gpt2-eventlab-device.ini simulator/gpu-simulator/bin/release/accel-sim.out > full-source.sha256
bash source/run_replay.sh "$BASE/full-run" 86400 > full-launcher-run.log 2>&1
python3 source/collect_metrics.py --capture "$OLD/capture-v2" --replay "$OLD/replay" \
 --run "$BASE/full-run" --reference "$OLD/native-reference" --output "$BASE/summary.json" > summary.log 2>&1
