"""Deterministic, fully covered page-0 writes for the one-Bank experiment."""
from __future__ import annotations

import argparse
from pathlib import Path

PAGE_BYTES = 4096
FRAGMENT_BYTES = 64
FRAGMENTS = PAGE_BYTES // FRAGMENT_BYTES
PAGES_PER_BLOCK = 8
ORDERS = ("dlu_major", "fragment_round_robin")


def records(dlus: int, order: str) -> list[tuple[int, int]]:
    if type(dlus) is not int or not 1 <= dlus <= 1024:
        raise ValueError("dlus must be an integer in [1, 1024]")
    if order not in ORDERS:
        raise ValueError(f"unknown order: {order}")
    pairs = ((b, f) for b in range(dlus) for f in range(FRAGMENTS)) if order == "dlu_major" else (
        (b, f) for f in range(FRAGMENTS) for b in range(dlus))
    # IDs and addresses remain identical across orderings. Only arrival order changes.
    return [(b * FRAGMENTS + f + 1, b * PAGES_PER_BLOCK * PAGE_BYTES + f * FRAGMENT_BYTES)
            for b, f in pairs]


def validate_coverage(rows: list[tuple[int, int]], dlus: int) -> None:
    expected = set(records(dlus, "dlu_major"))
    if len(rows) != dlus * FRAGMENTS or set(rows) != expected:
        raise ValueError("trace must cover every page-0 fragment exactly once with stable IDs")


def write_workload(path: Path, dlus: int, order: str) -> None:
    rows = records(dlus, order)
    validate_coverage(rows, dlus)
    text = "HBF_EVENTLAB_WORKLOAD 1\n" + "".join(
        f"WRITE {identity} {address} 64 content=1 at=0\n" for identity, address in rows)
    path.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dlus", type=int, default=64)
    parser.add_argument("--order", choices=ORDERS, default="fragment_round_robin")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_workload(args.output, args.dlus, args.order)
