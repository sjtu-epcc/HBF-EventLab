"""Output and interval utilities; all time values are integer simulator ticks (ps)."""
from __future__ import annotations

from collections import Counter
import csv
import hashlib
import json
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def csv_rows(path: Path, rows: list[dict], fields: list[str] | None = None) -> None:
    fields = fields or list(dict.fromkeys(k for r in rows for k in r))
    with path.open("w", encoding="utf-8", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def interval_curve(intervals: list[tuple[int, int | None]], stop: int) -> tuple[dict[int, int], int]:
    changes = Counter({0: 0, stop: 0})
    area = 0
    for start, end in intervals:
        until = stop if end is None else end
        if type(start) is not int or type(until) is not int or not 0 <= start <= until <= stop:
            raise ValueError("invalid resource interval")
        changes[start] += 1
        if end is not None:
            changes[end] -= 1
        area += until - start
    curve, active = {}, 0
    for at in sorted(changes):
        active += changes[at]
        if active < 0:
            raise ValueError("negative occupancy")
        curve[at] = active
    return curve, area


def ratio(n: int | float, d: int) -> float | None:
    return n / d if d else None
