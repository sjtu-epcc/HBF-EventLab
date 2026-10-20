"""Two independent block-start DLUs with identical records across Host policies."""
from __future__ import annotations

from pathlib import Path

POLICIES = ("fcfs", "fill_open")
SHAPES = ("batched", "sparse")
PAGE_BYTES = 4096
BLOCK_BYTES = 8 * PAGE_BYTES


def records(shape: str) -> list[dict]:
    if shape not in SHAPES:
        raise ValueError("unknown arrival shape")
    # A-first-half, B-first-half, A-second-half, B-second-half.
    # IDs and file order stay the same in BOTH shapes and policies.
    chunks = (("A", 0, 0), ("B", BLOCK_BYTES, 0),
              ("A", 0, 32), ("B", BLOCK_BYTES, 32))
    rows = []
    for batch, (page, base, first) in enumerate(chunks):
        for offset in range(first, first + 32):
            index = len(rows)
            at = (0, 2_000_000, 2_000_000, 4_000_000)[batch] if shape == "batched" else index * 100_000
            rows.append(dict(id=index + 1, page=page, address=base + offset * 64,
                             content=1, at=at))
    return rows


def write_workload(path: Path, shape: str) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as out:
        out.write("HBF_EVENTLAB_WORKLOAD 1\n")
        for r in records(shape):
            out.write(f'WRITE {r["id"]} {r["address"]} 64 content={r["content"]} at={r["at"]}\n')


def configuration(policy: str) -> str:
    if policy not in POLICIES:
        raise ValueError("unknown Host policy")
    return f"""# Minimal arrival-shape x Host-policy diagnostic. Tick=1ps.
[device]
ucie_speed_grade = 1
[device.geometry]
host_channels = 1
dies_per_channel = 1
banks_per_die = 1
pages_per_block = 8
blocks_per_bank = 4
spare_blocks_per_bank = 1
[device.capabilities]
mocs_supported = 256
max_pending_write_dlus = 8
read_burst = false
write_burst = false
[device.selected]
mocs_selected = 256
assembly_timeout_ps = 10000000
zone_size_dlus = auto
[address]
global_base = 0
interleave_bytes = 64
[host]
send_policy = {policy}
write_mode = fragment64
max_open_dlus_per_channel = 2
retained_fragments = 128
max_logical_fragments = 128
[host.retry]
max_pending_retries = 0
max_timeout_retries = 0
[host.recovery]
max_replay_passes = 0
[execution.media_timing]
erase_ps = 1000000
program_ps = 500000
[execution.nand_scheduler]
policy = fcfs
[execution.ucie_scheduler]
policy = fifo
[run]
max_tick_ps = 100000000
total_events = 1000000
"""
