#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261103-gpt2-updated
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > smoke-launcher.rc' EXIT
test "$(cat gpu-validation.rc)" = 0
bash source/build_simulator.sh > simulator-build.log 2>&1
git -C EventLab rev-parse HEAD > source-commit.txt
sha256sum source/hbf_eventlab_bridge.cc source/run_replay.sh source/CMakeLists.txt \
 adapter/src/gpu/page_controller.cpp > source-files.sha256
export HBF_DIAG_OUTPUT="$BASE/smoke-profile.jsonl"
bash source/run_replay.sh "$BASE/smoke-120" 120 > smoke-launcher-run.log 2>&1
