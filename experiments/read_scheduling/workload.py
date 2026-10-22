"""Finite cold-read diagnostics. All addresses are Global bytes, all times ps."""
from pathlib import Path
from experiments.common.model import PAGE_BYTES, FRAGMENT_BYTES

PAGES_PER_BLOCK = 8
MOCS_SELECTED = 256
PENDING_LIMIT = 8

START = 10_000_000
POLICIES = ("fcfs", "read_priority")


def cases() -> list[dict]:
    result = [dict(name=f"competition-w{wait}-{policy}", shape="competition", policy=policy,
                   wait=wait, threshold=0, hints="all_regular")
              for wait in (0, 50_000, 1_000_000) for policy in POLICIES]
    result += [dict(name=f"nonpreemptive-{policy}", shape="nonpreemptive", policy=policy,
                    wait=0, threshold=0, hints="all_regular") for policy in POLICIES]
    for name, shape, threshold, hints in (
        ("timer", "four", 0, "all_regular"),
        ("threshold", "four", 4, "all_regular"),
        ("tail", "three", 4, "all_regular"),
        ("single", "single", 4, "all_regular"),
        ("batch", "single", 4, "tail_batch"),
        ("cache-removal", "same_page", 2, "all_regular"),
    ):
        result.append(dict(name=name, shape=shape, policy="fcfs", wait=200_000,
                           threshold=threshold, hints=hints))
    return result


def records(shape: str, pages_per_block: int = PAGES_PER_BLOCK) -> list[dict]:
    # Preload four pages of block 0 through the real write path. No hidden NAND image.
    rows = [dict(id=1, op="WRITE", address=0, bytes=4*PAGE_BYTES, at=0, content=1, phase="warmup")]
    if shape in ("competition", "nonpreemptive"):
        rows += [dict(id=2, op="WRITE", address=pages_per_block*PAGE_BYTES, bytes=PAGE_BYTES, at=START, content=1, phase="measure"),
                 dict(id=3, op="READ", address=0, bytes=FRAGMENT_BYTES,
                      at=START+(200_000 if shape=="competition" else 1_200_000), phase="measure")]
    else:
        sizes = {"four":4, "three":3, "single":1, "same_page":3}
        if shape not in sizes:
            raise ValueError("unknown read scheduling shape")
        for i in range(sizes[shape]):
            rows.append(dict(id=2+i, op="READ", address=i*FRAGMENT_BYTES if shape=="same_page" else i*PAGE_BYTES,
                             bytes=FRAGMENT_BYTES, at=START+i*40_000, phase="measure"))
    return rows


def write_workload(path: Path, shape: str) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as out:
        out.write("HBF_EVENTLAB_WORKLOAD 1\n")
        for r in records(shape):
            extra = "content=1" if r["op"]=="WRITE" else "expect=1 after=1"
            out.write(f'{r["op"]} {r["id"]} {r["address"]} {r["bytes"]} at={r["at"]} {extra}\n')


def configuration(case: dict) -> str:
    if case["policy"] not in POLICIES or case["wait"]<0 or case["threshold"]<0:
        raise ValueError("invalid experiment treatment")
    return f"""# Functional, uncalibrated; Tick=1ps. Host policy and workload fixed.
[device.geometry]
host_channels = 1
dies_per_channel = 1
banks_per_die = 1
pages_per_block = {PAGES_PER_BLOCK}
blocks_per_bank = 4
spare_blocks_per_bank = 1
[device.capabilities]
mocs_supported = {MOCS_SELECTED}
max_pending_write_dlus = {PENDING_LIMIT}
read_burst = false
write_burst = false
[device.selected]
mocs_selected = {MOCS_SELECTED}
assembly_timeout_ps = 10000000
zone_size_dlus = auto
[address]
global_base = 0
interleave_bytes = {FRAGMENT_BYTES}
[host]
send_policy = fcfs
write_mode = fragment64
read_hint_mode = {case['hints']}
max_open_dlus_per_channel = 4
retained_fragments = 512
max_logical_fragments = 512
[host.retry]
max_pending_retries = 0
max_timeout_retries = 0
[host.recovery]
max_replay_passes = 0
[execution.media_timing]
sense_ps = 100000
erase_ps = 1000000
program_ps = 500000
cache_access_ps = 100
[execution.read_dispatch]
policy = window
wait_ps = {case['wait']}
threshold = {case['threshold']}
[execution.nand_scheduler]
policy = {case['policy']}
[execution.ucie_scheduler]
policy = fifo
[run]
max_tick_ps = 100000000
total_events = 1000000
"""
