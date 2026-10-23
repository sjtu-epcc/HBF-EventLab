"""Finite, deliberately roomy first-group interleave/arrival experiment."""
from __future__ import annotations

from itertools import product
from experiments.common.model import PAGE_BYTES, FRAGMENT_BYTES

PAGES_PER_BLOCK = 8
MOCS_SELECTED = 1024
PENDING_LIMIT = 16

DEFAULTS = {
    "channels": 4,
    "sizes_bytes": [16384, 131072],
    "interleave_bytes": [64, 256, 1024, 4096],
    "delta_ps": [0, 1000, 4000, 16000, 32000, 50000, 128000, 160000],
    "assembly_timeout_ps": 10000000,
    "max_tick_ps": 1000000000,
    "total_events": 10000000,
}


def validate_plan(value: dict) -> dict:
    if not isinstance(value, dict) or set(value) - set(DEFAULTS):
        raise ValueError("unknown plan field or non-object plan")
    p = DEFAULTS | value
    for key in ("channels", "assembly_timeout_ps", "max_tick_ps", "total_events"):
        if type(p[key]) is not int or not 0 < p[key] < 2**63:
            raise ValueError(f"{key}: expected positive integer below 2^63")
    if p["channels"] not in (1, 2, 4, 8):
        raise ValueError("this Grade-1 diagnostic supports 1/2/4/8 channels")
    for key in ("sizes_bytes", "interleave_bytes", "delta_ps"):
        xs = p[key]
        if not isinstance(xs, list) or not xs or any(type(x) is not int or not 0 <= x < 2**63 for x in xs):
            raise ValueError(f"{key}: expected nonempty integer list")
        if len(set(xs)) != len(xs):
            raise ValueError(f"{key}: duplicate values")
    group = p["channels"] * PAGE_BYTES
    if any(n == 0 or n % group or n > group * PAGES_PER_BLOCK for n in p["sizes_bytes"]):
        raise ValueError("size must cover whole channel/page groups, at most one 8-page block per channel")
    if any(i not in (64, 128, 256, 512, 1024, 2048, 4096) for i in p["interleave_bytes"]):
        raise ValueError("interleave must be a power of two from 64 through 4096 bytes")
    if (max(p["sizes_bytes"]) // FRAGMENT_BYTES - 1) * max(p["delta_ps"]) >= 2**63:
        raise ValueError("release schedule exceeds the harness integer bound")
    return p


def cases(p: dict) -> list[dict]:
    return [dict(size_bytes=n, interleave_bytes=i, delta_ps=d)
            for n, i, d in product(p["sizes_bytes"], p["interleave_bytes"], p["delta_ps"])]


def case_name(c: dict) -> str:
    return f'n{c["size_bytes"]}-i{c["interleave_bytes"]}-d{c["delta_ps"]}'


def configuration(p: dict, c: dict) -> str:
    # Fixed budgets across all matrix cells, including both input sizes. At most
    # 512 original commands / 8 original DLUs per channel; no retry/fault traffic.
    return f"""# Interleave x release interval; functional, uncalibrated timing.
[device]
ucie_speed_grade = 1
[device.geometry]
host_channels = {p['channels']}
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
assembly_timeout_ps = {p['assembly_timeout_ps']}
zone_size_dlus = auto
[address]
global_base = 0
interleave_bytes = {c['interleave_bytes']}
[host]
send_policy = fcfs
write_mode = fragment64
max_open_dlus_per_channel = {PENDING_LIMIT}
retained_fragments = 65536
max_logical_fragments = 65536
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
max_tick_ps = {p['max_tick_ps']}
total_events = {p['total_events']}
"""
