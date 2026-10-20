"""Small explicit parameter matrix; device validation remains authoritative."""
from __future__ import annotations

import itertools
from workload import ORDERS, PAGES_PER_BLOCK

DEFAULTS = {
    "dlus": 64,
    "orders": list(ORDERS),
    "open_dlus": [1, 2, 4, 5, 8, 9, 16, 17, 32],
    "mocs_selected": [256, 512, 1024],
    "pending_limits": [4, 8, 16, 64],
    "program_ps": [500000, 2000000, 8000000],
    "erase_ps": 1000000,
    "assembly_timeout_ps": 10000000,
    "max_tick_ps": 1000000000000,
    "total_events": 10000000,
}


def validate_plan(value: dict) -> dict:
    if not isinstance(value, dict) or set(value) - set(DEFAULTS):
        raise ValueError("plan contains unknown fields or is not an object")
    p = DEFAULTS | value
    for key in ("dlus", "erase_ps", "assembly_timeout_ps", "max_tick_ps", "total_events"):
        if type(p[key]) is not int or not 0 < p[key] < 2**63:
            raise ValueError(f"{key} must be a positive integer below 2^63")
    if p["dlus"] > 1024:
        raise ValueError("this diagnostic harness limits dlus to 1024")
    for key in ("orders", "open_dlus", "mocs_selected", "pending_limits", "program_ps"):
        xs = p[key]
        if not isinstance(xs, list) or not xs:
            raise ValueError(f"{key} must be a nonempty list")
        if key == "orders":
            if any(not isinstance(x, str) or x not in ORDERS for x in xs):
                raise ValueError("unknown workload order")
        elif any(type(x) is not int or not 0 < x < 2**63 for x in xs):
            raise ValueError(f"{key} must contain positive integers below 2^63")
        if len(set(xs)) != len(xs):
            raise ValueError(f"duplicate values in {key}")
    if any(x > p["dlus"] for x in p["open_dlus"]):
        raise ValueError("open_dlus must not exceed the number of independent blocks")
    if any(x != 1 and (x < 256 or x > 16384 or x & (x - 1)) for x in p["mocs_selected"]):
        raise ValueError("MOCS must be 1 or a power of two in [256, 16384]")
    return p


def cases(p: dict) -> list[dict]:
    keys = ("order", "open_dlus", "mocs_selected", "pending_limit", "program_ps")
    rows = itertools.product(p["orders"], p["open_dlus"], p["mocs_selected"], p["pending_limits"], p["program_ps"])
    return [dict(zip(keys, row)) for row in rows]


def case_name(c: dict) -> str:
    return (f'{c["order"]}-o{c["open_dlus"]}-c{c["mocs_selected"]}'
            f'-p{c["pending_limit"]}-t{c["program_ps"]}')


def configuration(p: dict, c: dict) -> str:
    # Do not silently change the supported MOCS or geometry between scan cells.
    return f"""# Synthetic capacity diagnostic, functional timing, not calibrated.
[device.geometry]
host_channels = 1
dies_per_channel = 1
banks_per_die = 1
pages_per_block = {PAGES_PER_BLOCK}
blocks_per_bank = {p['dlus']}
spare_blocks_per_bank = 1
[device.capabilities]
mocs_supported = {max(p['mocs_selected'])}
max_pending_write_dlus = {c['pending_limit']}
read_burst = false
write_burst = false
[device.selected]
mocs_selected = {c['mocs_selected']}
assembly_timeout_ps = {p['assembly_timeout_ps']}
zone_size_dlus = auto
[address]
global_base = 0
interleave_bytes = 64
[host]
send_policy = fcfs
write_mode = fragment64
max_open_dlus_per_channel = {c['open_dlus']}
retained_fragments = {p['dlus'] * 64}
max_logical_fragments = {p['dlus'] * 64}
[host.retry]
max_pending_retries = 0
max_timeout_retries = 0
[execution.media_timing]
program_ps = {c['program_ps']}
erase_ps = {p['erase_ps']}
[execution.nand_scheduler]
policy = fcfs
[execution.ucie_scheduler]
policy = fifo
[run]
max_tick_ps = {p['max_tick_ps']}
total_events = {p['total_events']}
"""
