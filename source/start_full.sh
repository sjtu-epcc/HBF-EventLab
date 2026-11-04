#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261103-gpt2-updated
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > full-launcher.rc' EXIT
test "$(cat gpu-validation.rc)" = 0
test ! -e full-run
unset HBF_DIAG_OUTPUT
bash source/run_replay.sh "$BASE/full-run" 14400 > full-launcher-run.log 2>&1
python3 source/collect_metrics.py --capture "$OLD/capture-v2" --replay "$OLD/replay" \
 --run "$BASE/full-run" --reference "$OLD/native-reference" --output "$BASE/summary.json" > summary.log 2>&1
