"""Read existing simulator observations; never reconstruct assembly state from accepts."""
from __future__ import annotations

import bisect
import csv
from collections import Counter, defaultdict
from pathlib import Path

from workload import FRAGMENTS, PAGE_BYTES, PAGES_PER_BLOCK


def ratio(n: int | float, d: int | float) -> float | None:
    return n / d if d else None


def csv_rows(path: Path, rows: list[dict], fields: list[str] | None = None) -> None:
    fields = fields or list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def interval_curve(intervals: list[tuple[int, int | None]], stop: int) -> tuple[dict, dict]:
    """Half-open occupancy, collapsed at equal Ticks; no fabricated instantaneous peaks."""
    changes = Counter({0: 0, stop: 0})
    area = 0
    for start, finish in intervals:
        end = stop if finish is None else finish
        if not 0 <= start <= end <= stop:
            raise ValueError("invalid interval or observation boundary")
        changes[start] += 1
        if finish is not None:
            changes[finish] -= 1
        area += end - start
    current, peak, full_ticks, integral = 0, 0, 0, 0
    curve = {}
    last = 0
    for at in sorted(changes):
        integral += current * (at - last)
        if current:
            full_ticks += at - last
        current += changes[at]
        if current < 0:
            raise ValueError("negative occupancy")
        curve[at] = current
        peak = max(peak, current)
        last = at
    if integral != area:
        raise ValueError("occupancy integral mismatch")
    return curve, {"peak_tick_boundary": peak, "area": area, "mean": ratio(area, stop),
                   "busy_ticks": full_ticks, "current": current}


def analyze(data: dict, dlus: int, selected: int, exit_code: int, directory: Path) -> dict:
    if data.get("schema_version") != 2 or "bank_operations" not in data:
        raise ValueError("need schema 2 statistics including bank_operations; rebuild the simulator")
    stop = data["at"]
    if type(stop) is not int or stop < 0 or data.get("time_unit") != "tick":
        raise ValueError("invalid statistics time unit/boundary")
    if exit_code not in (0, 2, 3, 4):
        raise ValueError(f"simulator input/model failure, exit {exit_code}")
    expected_stop = {0: "Completed", 2: "Completed", 3: "Stalled", 4: "LimitReached"}[exit_code]
    if data["stop_reason"] != expected_stop:
        raise ValueError("exit code and statistics stop reason disagree")
    if data["summary"]["replay_attempts"] or data["summary"]["retried_unit_attempts"]:
        raise ValueError("first-round scan must not contain recovery/retries")
    groups = defaultdict(list)
    seen = set()
    for w in data["logical_work"]:
        address = w["local_byte"]
        if w["channel"] != 0 or w["content"] != 1 or address % 64 or address in seen:
            raise ValueError("unexpected or duplicated logical footprint")
        seen.add(address)
        dlu = address // PAGE_BYTES
        if dlu % PAGES_PER_BLOCK or dlu // PAGES_PER_BLOCK >= dlus:
            raise ValueError("logical footprint is not page 0 of the planned blocks")
        if w["result"] not in ("success", "failed", "pending"):
            raise ValueError("unknown logical outcome")
        groups[dlu].append(w)
    if len(groups) != dlus or any(len(ws) != FRAGMENTS for ws in groups.values()):
        raise ValueError("logical output does not cover the complete planned workload")
    outcomes = Counter()
    pages = []
    for dlu, ws in sorted(groups.items()):
        counts = Counter(w["result"] for w in ws)
        # Any unfinished fragment makes the page censored, even if another already failed.
        result = "pending" if counts["pending"] else "success" if counts["success"] == FRAGMENTS else "failed"
        outcomes[result] += 1
        pages.append({"channel": 0, "local_dlu": dlu, "result": result,
                      "successful_fragments": counts["success"], "failed_fragments": counts["failed"],
                      "unfinished_fragments": counts["pending"],
                      "finished_at": None if counts["pending"] else max(w["finished"] for w in ws)})
    if exit_code == 0 and outcomes["success"] != dlus:
        raise ValueError("successful CLI exit contains unsuccessful logical work")
    if exit_code in (0, 2) and outcomes["pending"]:
        raise ValueError("completed simulation contains unfinished logical work")
    csv_rows(directory / "dlus.csv", pages)
    attempts = data["attempts"]
    statuses = Counter(a["status"] for a in attempts if a["delivered"] is not None)
    if any(a["op"] != "write" or a["channel"] != 0 or a["bytes"] != 64 for a in attempts):
        raise ValueError("expected only single-channel fragment64 writes")
    mocs, m = interval_curve([(a["accepted"], a["delivered"]) for a in attempts if a["accepted"] is not None], stop)
    if m["peak_tick_boundary"] > selected:
        raise ValueError("observed MOCS exceeds Selected")
    times = sorted(mocs)
    mocs_full = sum(b - a for a, b in zip(times, times[1:]) if mocs[a] == selected)
    ops = data["bank_operations"]
    bank_intervals = defaultdict(list)
    for op in ops:
        if (op["channel"], op["die"], op["bank"]) != (0, 0, 0) or op["kind"] not in ("erase", "program"):
            raise ValueError("unexpected Bank target or operation")
        bank_intervals[op["kind"]].append((op["issued_at"], op["completed_at"]))
    all_ops = sorted(x for xs in bank_intervals.values() for x in xs)
    for left, right in zip(all_ops, all_ops[1:]):
        if left[1] is None or left[1] > right[0]:
            raise ValueError("one-Bank operations overlap")
    bank, b = interval_curve(all_ops, stop)
    kinds = {kind: interval_curve(bank_intervals[kind], stop)[1] for kind in ("erase", "program")}
    if len(data["pending_channels"]) != 1 or data["pending_channels"][0]["channel"] != 0:
        raise ValueError("expected one Pending channel")
    pending = data["pending_channels"][0]
    timeline = data["pending_timeline"]
    if not timeline or any(x["channel"] != 0 or not 0 <= x["at"] <= stop for x in timeline):
        raise ValueError("invalid Pending timeline")
    if timeline != sorted(timeline, key=lambda x: (x["at"], x["sequence"])):
        raise ValueError("unordered Pending observations")
    pending_curve = {x["at"]: x["counts"] for x in timeline}
    if 0 not in pending_curve:
        raise ValueError("missing initial Pending observation")
    csv_rows(directory / "pending.csv", [{"at_ps": x["at"], "sequence": x["sequence"],
                                         "channel": x["channel"], **x["counts"]} for x in timeline])
    pending_times = sorted(pending_curve)
    contexts = data["dlu_contexts"]
    termination = Counter(d["termination"] for d in contexts)
    csv_rows(directory / "contexts.csv", [{k: v for k, v in d.items() if k not in ("members", "progress")}
                                          for d in contexts], None if contexts else ["channel", "local_dlu", "generation",
                                          "opened_at", "ready_at", "program_started_at", "retired_at", "termination"])
    progress = [{"channel": d["channel"], "local_dlu": d["local_dlu"], "generation": d["generation"], **p}
                for d in contexts for p in d["progress"]]
    csv_rows(directory / "fragments.csv", progress, ["channel", "local_dlu", "generation", "at", "sequence", "change",
                                                    "phase", "received_fragments", "command"])
    csv_rows(directory / "bank.csv", ops, ["ticket", "kind", "channel", "die", "bank", "block", "page",
                                          "issued_at", "completed_at", "observed_busy_ticks"])
    curve_rows = []
    mt, bt = sorted(mocs), sorted(bank)
    for at in sorted(set(mt) | set(bt) | set(pending_times)):
        pc = pending_curve[pending_times[bisect.bisect_right(pending_times, at) - 1]]
        curve_rows.append({"at_ps": at, "channel": 0, "mocs": mocs[mt[bisect.bisect_right(mt, at) - 1]],
                           "bank_busy": bank[bt[bisect.bisect_right(bt, at) - 1]], **pc})
    csv_rows(directory / "resources.csv", curve_rows)
    # These are cross-checks against the existing state-owner observations, not another assembly model.
    for field in ("total", "collecting", "ready_waiting", "programming"):
        integral = sum(a[field] * (z["at_ps"] - a["at_ps"]) for a, z in zip(curve_rows, curve_rows[1:]))
        if integral != pending["area_context_ticks"][field]:
            raise ValueError("Pending curve and collector integral disagree")
    if exit_code in (0, 2) and (m["current"] or b["current"] or pending["current"]["total"]):
        raise ValueError("completed simulation still owns temporary resources")
    result = {
        "stop_reason": data["stop_reason"], "exit_code": exit_code,
        "simulation_end_ps": stop,
        "logical_completion_ps": None if outcomes["pending"] else max(p["finished_at"] for p in pages),
        "planned_dlus": dlus, "successful_dlus": outcomes["success"], "failed_dlus": outcomes["failed"],
        "unfinished_dlus": outcomes["pending"], "dlu_success_ratio": ratio(outcomes["success"], dlus),
        "opened_contexts": len(contexts), "timeout_contexts": termination["assembly_timeout"],
        "timeout_context_ratio": ratio(termination["assembly_timeout"], len(contexts)),
        "pending_limit_responses": statuses[4], "timeout_responses": statuses[5],
        "pending_limit_affected_dlus": len({a["local_byte"] // PAGE_BYTES for a in attempts if a["status"] == 4}),
        "attempts_handed": len(attempts), "attempts_accepted": sum(a["accepted"] is not None for a in attempts),
        "commands_delivered": sum(a["delivered"] is not None for a in attempts),
        "unhanded_fragments": dlus * FRAGMENTS - len(attempts),
        "mocs_peak_tick_boundary": m["peak_tick_boundary"], "mocs_mean": m["mean"], "mocs_full_ps": mocs_full,
        "pending_peak": pending["peak"]["total"], "pending_mean": pending["time_weighted_mean"]["total"],
        "pending_full_ps": pending["at_limit_ticks"],
        "collecting_mean": pending["time_weighted_mean"]["collecting"],
        "ready_waiting_mean": pending["time_weighted_mean"]["ready_waiting"],
        "programming_mean": pending["time_weighted_mean"]["programming"],
        "bank_busy_ps": b["area"], "bank_utilization": ratio(b["area"], stop),
        "erase_busy_ps": kinds["erase"]["area"], "program_busy_ps": kinds["program"]["area"],
        "first_ready_ps": min((d["ready_at"] for d in contexts if d["ready_at"] is not None), default=None),
    }
    return result
