#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261106-gpt2-k342-mshr
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > k342-mshr512-long-launcher.rc' EXIT
test ! -e k342-mshr512-long
sha256sum source/hbf_eventlab_bridge.cc config/gpt2-eventlab-device.ini \
 simulator/gpu-simulator/bin/release/accel-sim.out > k342-mshr512-long-source.sha256
bash source/run_kernel342_mshr.sh "$BASE/k342-mshr512-long" 7200 512 1 > k342-mshr512-long-launcher.log 2>&1
