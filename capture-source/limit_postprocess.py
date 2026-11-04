"""Restrict the verified experiment postprocessor and its descendants to 2 CPUs."""
import os
import sys
import time
from pathlib import Path

BASE='/home/server/hbf-experiments/20261028-gpt2-incremental'

def state(pid):
    try:
        fields=Path('/proc/%d/stat'%pid).read_text().rsplit(') ',1)[1].split()
        return int(fields[1]), fields[19]
    except (FileNotFoundError,ProcessLookupError):return None

root=int(sys.argv[1])
identity=state(root)
command=Path('/proc/%d/cmdline'%root).read_bytes().split(b'\0')
assert command[0].decode()==BASE+'/HBF-Sim-eventlab/util/tracer_nvbit/tracer_tool/traces-processing/post-traces-processing'
assert command[1].decode().startswith(BASE+'/capture-v2/traces/kernelslist_ctx_')
assert identity
# First constrain the parent, then descendants; future children inherit affinity.
for iteration in range(3):
    if state(root)!=identity:break
    snapshot={int(p.name):state(int(p.name)) for p in Path('/proc').iterdir() if p.name.isdigit()}
    selected={root}
    while True:
        more={pid for pid,info in snapshot.items() if info and info[0] in selected}
        if more.issubset(selected):break
        selected.update(more)
    for pid in [root]+sorted(selected-{root}):
        old=snapshot.get(pid)
        if not old or state(pid)!=old:continue
        try:
            for task in Path('/proc/%d/task'%pid).iterdir():
                if state(pid)!=old:break
                os.sched_setaffinity(int(task.name), {0,1})
            print('limited',pid,old[1],flush=True)
        except (FileNotFoundError,ProcessLookupError):pass
    time.sleep(0.1)
