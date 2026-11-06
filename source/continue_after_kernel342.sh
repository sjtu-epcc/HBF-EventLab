#!/usr/bin/env bash
set -euo pipefail
BASE=/home/server/hbf-experiments/20261106-gpt2-k342-mshr
OLD=/home/server/hbf-experiments/20261028-gpt2-incremental
cd "$BASE"
trap 'rc=$?; printf "%s\n" "$rc" > conditional-full-launcher.rc' EXIT
while test ! -f k342-mshr512-long/run.rc; do sleep 30; done
while test ! -f k342-mshr512-long-launcher.rc; do sleep 5; done
test "$(cat k342-mshr512-long/run.rc)" = 0
launcher_rc="$(cat k342-mshr512-long-launcher.rc)"
if test "$launcher_rc" != 0; then
  # The already-finished launcher has a stray CR-only final line after the
  # simulator command. Bash returned 127 for that empty-looking line; the
  # run_sim.sh process itself wrote run.rc=0 and is audited below.
  test "$launcher_rc" = 127
  python3 - <<'PY'
from pathlib import Path
if not Path('source/start_kernel342_mshr512.sh').read_bytes().endswith(b'\n\r\n'):
    raise SystemExit('unexplained nonzero kernel launcher exit')
PY
fi
python3 - <<'PY'
import re
from pathlib import Path
from source.audit_trace import validate

root = Path('k342-mshr512-long')
log = (root / 'run.log').read_text(errors='replace')
completed = re.findall(r'kernel_launch_uid\s*=\s*(\d+)\s+.*?gpu_tot_sim_cycle\s*=\s*(\d+)', log, re.S)
if len(completed) != 1 or completed[0][0] != '1':
    raise SystemExit('kernel 342 did not complete exactly once')
def latest(prefix):
    lines = [line for line in log.splitlines() if line.startswith(prefix + ' ')]
    if not lines:
        raise SystemExit('missing ' + prefix)
    return {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', lines[-1])}
gpu, device = latest('EVENTLAB_GPU'), latest('EVENTLAB_DEVICE')
if not (gpu['admitted'] == gpu['completed'] == gpu['returned'] > 0
        and gpu['failed'] == gpu['owned'] == 0 and device['drained'] == 1):
    raise SystemExit('kernel 342 HBF requests did not drain')
audit = validate(root / 'eventlab.csv')
if audit['requests'] != gpu['admitted']:
    raise SystemExit('kernel 342 lifecycle count mismatch')
print('kernel342 validated:', completed[0], gpu, device, audit, flush=True)
PY
test ! -e full-run
sha256sum source/hbf_eventlab_bridge.cc source/run_replay_mshr512.sh \
 source/continue_after_kernel342.sh adapter/src/gpu/page_controller.cpp \
 config/gpt2-eventlab-device.ini simulator/gpu-simulator/bin/release/accel-sim.out \
 > conditional-full-source.sha256
bash source/run_replay_mshr512.sh "$BASE/full-run" 172800 > conditional-full-run-launcher.log 2>&1
python3 source/collect_metrics.py --capture "$OLD/capture-v2" --replay "$OLD/replay" \
 --run "$BASE/full-run" --reference "$OLD/native-reference" --output "$BASE/summary.json" \
 > conditional-summary.log 2>&1
