"""Independent input/mapping oracle, checked against the simulator's emitted addresses."""
from __future__ import annotations

from collections import defaultdict
from pathlib import Path


def records(size_bytes: int, delta_ps: int) -> list[dict]:
    if type(size_bytes) is not int or size_bytes <= 0 or size_bytes % 64:
        raise ValueError("size must be a positive multiple of 64B")
    if type(delta_ps) is not int or delta_ps < 0 or (size_bytes // 64 - 1) * delta_ps >= 2**63:
        raise ValueError("invalid or overflowing release interval")
    return [dict(global_id=i + 1, global_byte=i * 64, scheduled_release=i * delta_ps)
            for i in range(size_bytes // 64)]


def map_address(address: int, channels: int, interleave: int) -> tuple[int, int]:
    stripe, offset = divmod(address, interleave)
    return stripe % channels, stripe // channels * interleave + offset


def release_span(channels: int, interleave: int, delta_ps: int) -> int:
    return (64 * channels - (channels - 1) * (interleave // 64) - 1) * delta_ps


def footprint(size_bytes: int, delta_ps: int, channels: int, interleave: int) -> list[dict]:
    if channels not in (1, 2, 4, 8) or interleave not in (64, 128, 256, 512, 1024, 2048, 4096):
        raise ValueError("unsupported diagnostic mapping")
    if size_bytes % (channels * 4096):
        raise ValueError("incomplete channel/page group")
    out, groups = [], defaultdict(list)
    for r in records(size_bytes, delta_ps):
        ch, local = map_address(r["global_byte"], channels, interleave)
        item = r | dict(channel=ch, local_byte=local, local_dlu=local // 4096)
        out.append(item)
        groups[ch, local // 4096].append(item)
    for rows in groups.values():
        if sorted(r["local_byte"] % 4096 for r in rows) != list(range(0, 4096, 64)):
            raise ValueError("input does not cover each local page exactly once")
        span = max(r["scheduled_release"] for r in rows) - min(r["scheduled_release"] for r in rows)
        if span != release_span(channels, interleave, delta_ps):
            raise ValueError("enumerated release schedule disagrees with analytical span")
    return out


def write_workload(path: Path, size_bytes: int, delta_ps: int) -> None:
    # One file per size/delta, shared byte-for-byte by all interleave settings.
    with path.open("x", encoding="utf-8", newline="\n") as out:
        out.write("HBF_EVENTLAB_WORKLOAD 1\n")
        for r in records(size_bytes, delta_ps):
            out.write(f'WRITE {r["global_id"]} {r["global_byte"]} 64 content=1 at={r["scheduled_release"]}\n')
