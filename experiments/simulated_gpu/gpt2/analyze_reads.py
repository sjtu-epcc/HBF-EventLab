"""Summarize a single-GPU EventLab read observation stream (one process per run)."""
import argparse
from collections import Counter, defaultdict
import json
from math import ceil
from read_order import ReadOrder


def wait_summary(values):
    if not values:
        return {"n": 0}
    ordered = sorted(values)
    return dict(n=len(values), positive=sum(v > 0 for v in values),
                mean_ps=sum(values)/len(values), p50_ps=ordered[ceil(len(values)*0.50)-1],
                p95_ps=ordered[ceil(len(values)*0.95)-1], max_ps=ordered[-1])


def summarize(lines):
    order = ReadOrder()
    counts = Counter()
    entered = set()
    served = set()
    services = {}
    ready = {}
    eligible = defaultdict(set)
    returned = {}
    histogram = Counter()
    waiting = defaultdict(list)
    commands = set()
    candidates = {}
    releases = Counter()
    hints = Counter()
    sm_cycles = defaultdict(Counter)
    mappings = set()
    linked = {}
    accepted = set()
    for line in lines:
        kind, *fields = line.split()
        if order.consume(kind, fields):
            continue
        if kind == "command":
            _, attempt, _, _, hint = fields
            commands.add(attempt)
            hints[hint] += 1
        elif kind == "link":
            attempt, global_id = fields
            linked[attempt] = global_id
        elif kind == "accepted":
            accepted.add(fields[1])
        elif kind == "mapping":
            _, global_id, _, merged = fields
            mappings.add(global_id)
            counts["sector_to_global_links"] += 1
            counts["merged_links"] += int(merged)
        elif kind == "enter":
            entered.add(fields[1])
        elif kind == "release":
            _, _, reason, size, *members = fields
            if len(members) != int(size) or not entered.issuperset(members):
                raise ValueError("invalid read window membership")
            releases[f"{reason}/{size}"] += 1
        elif kind == "service":
            _, attempt, service = fields
            if attempt in served or attempt not in commands:
                raise ValueError("duplicate service or missing read command")
            services[attempt] = service
            served.add(attempt)
            counts[service + ("_after_entry" if attempt in entered else "_without_entry")] += 1
        elif kind in ("egress-ready", "response-read-candidate", "response-issue", "write-response-issue"):
            tick, channel, attempt = fields
            tick = int(tick)
            if kind == "egress-ready":
                if attempt in ready or attempt in returned:
                    raise ValueError("duplicate read response readiness")
                ready[attempt] = (channel, tick)
            elif kind == "response-read-candidate":
                if attempt not in ready or ready[attempt][0] != channel or attempt in eligible[channel]:
                    raise ValueError("invalid response candidate")
                eligible[channel].add(attempt)
            else:
                options = eligible.pop(channel, set())
                if options:
                    histogram[len(options)] += 1
                if kind == "response-issue":
                    if attempt not in options:
                        raise ValueError("read response selected outside candidate set")
                    _, start = ready.pop(attempt)
                    if tick < start:
                        raise ValueError("response issued before ready")
                    returned[attempt] = tick-start
                    waiting[services.get(attempt, "other")].append(tick-start)
        elif kind == "prior-sense":
            counts["prior_sense_block_checks"] += 1
        elif kind == "sense-candidate":
            _, attempt, *page = fields
            candidates[attempt] = tuple(page)
        elif kind == "sense-selected":
            _, attempt, *page = fields
            if candidates.get(attempt) != tuple(page):
                raise ValueError("selected read missing from candidate set")
            counts["sense_decisions"] += 1
            counts["multiple_read_candidates"] += len(candidates) > 1
            counts["alternative_page_on_selected_bank"] += any(
                other[:3] == tuple(page[:3]) and other != tuple(page)
                for other in candidates.values()
            )
            candidates.clear()
        elif kind == "sm":
            _, kernel, _, warp_ready, issued, outstanding = map(int, fields)
            sample = sm_cycles[kernel]
            sample["active_sm_cycles"] += 1
            sample["no_scoreboard_ready"] += not warp_ready
            sample["ready_without_issue"] += warp_ready and not issued
            sample["no_ready_with_gpu_reads_outstanding"] += not warp_ready and outstanding > 0
        else:
            raise ValueError(f"unknown observation {kind}")
    if commands != accepted or commands != linked.keys() or set(linked.values()) != mappings:
        raise ValueError('incomplete read mappings or acceptance records (expected drained replay)')
    if candidates:
        raise ValueError("unterminated NAND decision")
    counts.update(read_attempts=len(commands), reads_entering_window=len(entered),
                  read_globals=len(mappings), attempts_without_cache_or_sense=len(commands-served))
    arbitration = None  # Older observation streams omit response arbitration.
    if returned or eligible or ready:
        if ready or eligible or returned.keys() != commands:
            raise ValueError("incomplete read response arbitration")
        arbitration = dict(candidate_histogram=histogram, wait=wait_summary(returned.values()),
                           wait_by_service={k: wait_summary(v) for k, v in waiting.items()})
    return dict(read_order=order.finish() if order.choices or order.accepted or order.open or order.intervals else None, response_arbitration=arbitration, counts=counts, hints=hints, release_reason_and_size=releases,
                active_sm_cycles_by_kernel=sm_cycles)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("observations")
    args = parser.parse_args()
    with open(args.observations, encoding="utf-8") as source:
        print(json.dumps(summarize(source), indent=2, sort_keys=True))
