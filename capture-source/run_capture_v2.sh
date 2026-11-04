#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261028-gpt2-incremental
trap 'launcher_rc=$?; printf "%s\n" "$launcher_rc" > "$BASE/capture-v2-launcher.exit_code"' EXIT
PYTHON=/home/server/anaconda3/envs/gpt2_kv/bin/python
TRACER=/tmp/hbf-complete-20261022/HBF-Sim-eventlab/util/tracer_nvbit/tracer_tool/tracer_tool.so
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1
export PYTHONPATH="$BASE/python-deps${PYTHONPATH:+:$PYTHONPATH}"
export PATH=/usr/local/cuda/bin:$PATH
mkdir -p "$BASE/bin"
nvcc -O3 -std=c++14 -shared -Xcompiler -fPIC -arch=sm_80 \
  "$BASE/source/embedding_cuda.cu" -o "$BASE/bin/embedding_cuda.so"
"$PYTHON" "$BASE/source/capture_gpt2.py" --embedding-lib "$BASE/bin/embedding_cuda.so" \
  --output "$BASE/native-embedding-reference" > "$BASE/native-embedding-reference.log" 2>&1
"$PYTHON" -c 'import json,sys; a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2])); assert a["generated_token_ids"]==b["generated_token_ids"], "embedding changed generated tokens"' \
  "$BASE/native-reference/result.json" "$BASE/native-embedding-reference/result.json"
mkdir "$BASE/capture-v2"
cd "$BASE/capture-v2"
taskset -c 0,1 timeout 7200 env LD_PRELOAD="$TRACER" ACTIVE_FROM_START=0 NO_EAGER_LOAD=1 TOOL_COMPRESS=1 \
  "$PYTHON" "$BASE/source/capture_gpt2.py" --capture --embedding-lib "$BASE/bin/embedding_cuda.so" --output metadata \
  > console.log 2>&1 &
capture_pid=$!
printf '%s\n' "$capture_pid" > capture.pid
python3 "$BASE/source/guard_capture.py" --pid "$capture_pid" --output memory_guard.json > memory-guard.log 2>&1 &
guard_pid=$!
set +e
wait -n "$capture_pid" "$guard_pid"
# These are direct children of this shell, never a PID discovered by name.
# If the monitor exits first, fail closed instead of leaving an unguarded run.
guard_failed=0
if ! kill -0 "$guard_pid" 2>/dev/null && kill -0 "$capture_pid" 2>/dev/null; then
  guard_failed=1
  kill -TERM "$capture_pid" 2>/dev/null
fi
wait "$capture_pid"
rc=$?
wait "$guard_pid"
guard_rc=$?
if [[ "$guard_failed" != 0 || "$guard_rc" != 0 ]]; then rc=125; fi
printf '%s\n' "$rc" > exit_code
set -e
if [[ "$rc" == 0 ]]; then bash "$BASE/source/finish_replay.sh"; fi
exit "$rc"
