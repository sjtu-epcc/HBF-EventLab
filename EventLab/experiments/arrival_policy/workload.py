"""Two independent block-start DLUs with identical records across Host policies."""
from __future__ import annotations

from pathlib import Path

POLICIES = ("fcfs", "fill_open")
SHAPES = ("batched", "sparse")
from experiments.common.model import PAGE_BYTES, FRAGMENT_BYTES, FRAGMENTS
PAGES_PER_BLOCK = 8
MOCS_SELECTED = 256
PENDING_LIMIT = 8
BLOCK_BYTES = PAGES_PER_BLOCK * PAGE_BYTES


def records(shape: str, pages_per_block: int = PAGES_PER_BLOCK) -> list[dict]:
    if shape not in SHAPES:
        raise ValueError("unknown arrival shape")
    # A-first-half, B-first-half, A-second-half, B-second-half.
    # IDs and file order stay the same in BOTH shapes and policies.
    block_bytes = pages_per_block * PAGE_BYTES
    chunks = (("A", 0, 0), ("B", block_bytes, 0),
              ("A", 0, 32), ("B", block_bytes, 32))
    rows = []
    for batch, (page, base, first) in enumerate(chunks):
        for offset in range(first, first + 32):
            index = len(rows)
            at = (0, 2_000_000, 2_000_000, 4_000_000)[batch] if shape == "batched" else index * 100_000
            rows.append(dict(id=index + 1, page=page, address=base + offset * FRAGMENT_BYTES,
                             content=1, at=at))
    return rows


def write_workload(path: Path, shape: str) -> None:
    with path.open("x", encoding="utf-8", newline="\n") as out:
        out.write("HBF_EVENTLAB_WORKLOAD 1\n")
        for r in records(shape):
            out.write(f'WRITE {r["id"]} {r["address"]} {FRAGMENT_BYTES} content={r["content"]} at={r["at"]}\n')


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
send_policy = {policy}
write_mode = fragment64
max_open_dlus_per_channel = 2
retained_fragments = {2 * FRAGMENTS}
max_logical_fragments = {2 * FRAGMENTS}
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
