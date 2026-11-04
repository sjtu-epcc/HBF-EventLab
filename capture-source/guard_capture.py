"""Stop only the verified capture process group if its host RAM budget is exceeded."""
import argparse
import json
import os
import signal
import time
from pathlib import Path

def record(pid):
    try:
        parts=Path('/proc/%d/stat'%pid).read_text().rsplit(') ',1)[1].split()
        return int(parts[2]),parts[19],int(parts[21])*os.sysconf('SC_PAGE_SIZE')
    except (FileNotFoundError,ProcessLookupError):return None

def await_verified_leader(pid):
    identity=None
    for _ in range(100):
        current=record(pid)
        if not current:return None
        if identity is None:identity=current[1]
        if current[1]!=identity:raise RuntimeError('leader identity changed during startup')
        try:command=Path('/proc/%d/cmdline'%pid).read_bytes()
        except (FileNotFoundError,ProcessLookupError):return None
        if current[0]==pid and b'/20261028-gpt2-incremental/source/capture_gpt2.py' in command:
            return current
        time.sleep(0.05)
    raise RuntimeError('refusing unverified process group after startup grace period')


def main():
    p=argparse.ArgumentParser();p.add_argument('--pid',type=int,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();initial=await_verified_leader(a.pid)
    if not initial:return
    print('GUARD_READY',a.pid,initial[1],flush=True)
    while True:
        leader=record(a.pid)
        if not leader or leader[1]!=initial[1]:return
        members=[]
        for item in Path('/proc').iterdir():
            if not item.name.isdigit():continue
            pid=int(item.name);r=record(pid)
            if r and r[0]==a.pid:members.append((pid,r))
        rss=sum(r[2] for _,r in members)
        available=next(int(line.split()[1])*1024 for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:'))
        if rss>24*(1<<30) or available<8*(1<<30):
            a.output.write_text(json.dumps(dict(reason='capture_memory_budget',rss_bytes=rss,available_bytes=available,pids=[pid for pid,_ in members]),indent=2))
            for pid,old in members:
                current=record(pid)
                if current and current[:2]==old[:2]:
                    try:os.kill(pid,signal.SIGTERM)
                    except ProcessLookupError:pass
            time.sleep(3)
            for pid,old in members:
                current=record(pid)
                if current and current[:2]==old[:2]:
                    try:os.kill(pid,signal.SIGKILL)
                    except ProcessLookupError:pass
            return
        time.sleep(5)

if __name__=='__main__':main()
