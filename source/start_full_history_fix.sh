#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261107-gpt2-retired-history
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > full-launcher.rc' EXIT
test "$(cat smoke-two-stage3/run.rc)" = 0
test "$(grep -c '^kernel_launch_uid' smoke-two-stage3/run.log)" = 2
test "$(grep -c 'EVENTLAB_HISTORY_RETIRED requests=' smoke-two-stage3/run.log)" = 2
grep '^EVENTLAB_GPU ' smoke-two-stage3/run.log | tail -n 1 | grep -q 'failed=0 owned=0'
grep '^EVENTLAB_DEVICE ' smoke-two-stage3/run.log | tail -n 1 | grep -q 'drained=1'
test ! -e full-run
sha256sum source/hbf_eventlab_bridge.cc adapter/src/gpu/page_controller.cpp \
 gpu-headers/hbf_eventlab/gpu/page_controller.hpp \
 config/gpt2-eventlab-device.ini source/run_replay_history_fix.sh \
 simulator/gpu-simulator/bin/release/accel-sim.out > full-source.sha256
bash source/run_replay_history_fix.sh "$BASE/full-run" 172800 > full-run-launcher.log 2>&1
python3 source/collect_metrics.py --capture "$OLD/capture-v2" --replay "$OLD/replay" \
 --run "$BASE/full-run" --reference "$OLD/native-reference" --output "$BASE/summary.json" \
 > summary.log 2>&1
