#!/usr/bin/env python3
"""Remap captured GPT-2 parameter reads into the configured HBF range."""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import lzma
import re
from pathlib import Path

HBF_BASE = 274877906944
PAGE_BYTES = 4096


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            result.update(block)
    return result.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    weights = json.loads((args.capture / "metadata" / "weights.json").read_text())
    weights.sort(key=lambda item: item["address"])
    cursor = HBF_BASE
    for weight in weights:
        cursor = (cursor + PAGE_BYTES - 1) // PAGE_BYTES * PAGE_BYTES
        weight["target"] = cursor
        cursor += weight["bytes"]
    for left, right in zip(weights, weights[1:]):
        if left["address"] + left["bytes"] > right["address"]:
            raise ValueError("overlapping non-tied parameter allocations")

    starts = [item["address"] for item in weights]
    args.output.mkdir(parents=True, exist_ok=False)
    names = [
        line
        for line in (args.capture / "traces" / "kernelslist.g").read_text().splitlines()
        if line.startswith("kernel")
    ]
    reports = []
    touched_pages: set[int] = set()
    total_mapped_lanes = 0
    mapped_by_weight = {item['address']: 0 for item in weights}

    for name in names:
        source = args.capture / "traces" / name
        destination = args.output / name
        versions: set[int] = set()
        mapped = 0
        with lzma.open(source, "rt") as reader, lzma.open(destination, "wt", preset=1) as writer:
            for line in reader:
                if line.startswith("-binary version"):
                    versions.add(int(line.split("=")[1]))
                tokens = line.split()
                if len(tokens) < 7 or not re.fullmatch("[0-9a-fA-F]+", tokens[0]):
                    writer.write(line)
                    continue
                opcode_index = 3 + int(tokens[2])
                opcode = tokens[opcode_index]
                width_index = opcode_index + 2 + int(tokens[opcode_index + 1])
                width = int(tokens[width_index])
                if not width:
                    writer.write(line)
                    continue
                mode = int(tokens[width_index + 1])
                address_index = width_index + 2
                lanes = bin(int(tokens[1], 16)).count("1")
                if mode == 0:
                    addresses = [int(value, 16) for value in tokens[address_index : address_index + lanes]]
                    end = address_index + lanes
                elif mode == 1:
                    addresses = [
                        int(tokens[address_index], 16) + lane * int(tokens[address_index + 1])
                        for lane in range(lanes)
                    ]
                    end = address_index + 2
                elif mode == 2:
                    addresses = [int(tokens[address_index], 16)]
                    for delta in tokens[address_index + 1 : address_index + lanes]:
                        addresses.append(addresses[-1] + int(delta))
                    end = address_index + lanes
                else:
                    raise ValueError(f"unsupported address compression mode {mode}")

                remapped = []
                changed = False
                for address in addresses:
                    index = bisect.bisect_right(starts, address) - 1
                    weight = weights[index] if index >= 0 else None
                    if weight and weight["address"] <= address < weight["address"] + weight["bytes"]:
                        if not opcode.startswith(("LDG", "LD.")):
                            raise ValueError(f"parameter accessed by non-load opcode {opcode}")
                        if address + width > weight["address"] + weight["bytes"]:
                            raise ValueError("parameter access crosses allocation boundary")
                        target = weight["target"] + address - weight["address"]
                        remapped.append(target)
                        touched_pages.update(range((target-HBF_BASE)//PAGE_BYTES,
                                                   (target+width-1-HBF_BASE)//PAGE_BYTES+1))
                        mapped_by_weight[weight['address']] += 1
                        mapped += 1
                        changed = True
                    else:
                        remapped.append(address)

                if changed:
                    shifts = {new - old for old, new in zip(addresses, remapped)}
                    if mode in (1, 2) and len(shifts) == 1:
                        tokens[address_index] = hex(remapped[0])
                    else:
                        tokens = (
                            tokens[: width_index + 1]
                            + ["0"]
                            + [hex(address) for address in remapped]
                            + tokens[end:]
                        )
                    line = " ".join(tokens) + "\n"
                writer.write(line)

        total_mapped_lanes += mapped
        reports.append(
            {
                "file": name,
                "binary_versions": sorted(versions),
                "mapped_lane_accesses": mapped,
                "input_sha256": digest(source),
                "output_sha256": digest(destination),
            }
        )
        print(name, sorted(versions), mapped, flush=True)

    (args.output / "kernelslist.g").write_text("".join(name + "\n" for name in names))
    image_pages = set()
    for weight in weights:
        weight['mapped_lane_accesses'] = mapped_by_weight[weight['address']]
        image_pages.update(range((weight['target']-HBF_BASE)//PAGE_BYTES,
                                (weight['target']+weight['bytes']-1-HBF_BASE)//PAGE_BYTES+1))
    (args.output / "image-pages.txt").write_text("".join(f"{page}\n" for page in sorted(image_pages)))
    manifest = {
        "hbf_base": HBF_BASE,
        "page_bytes": PAGE_BYTES,
        "weights": weights,
        "kernels": reports,
        "kernel_count": len(names),
        "mapped_lane_accesses": total_mapped_lanes,
        "touched_pages": len(touched_pages),
        "initialized_pages": len(image_pages),
        "mapped_weight_span_bytes": cursor - HBF_BASE,
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({key: manifest[key] for key in ("kernel_count", "mapped_lane_accesses", "touched_pages", "mapped_weight_span_bytes")}, indent=2))


if __name__ == "__main__":
    main()
