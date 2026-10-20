"""Validate the finite example and export observations, without simulating assembly."""
from __future__ import annotations

from bisect import bisect_right
from itertools import groupby
from pathlib import Path

from experiments.interleave_arrival.io_utils import csv_rows, interval_curve
from .workload import BLOCK_BYTES, records


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def index(rows: list[dict], key: str) -> dict:
    result = {r[key]: r for r in rows}
    require(len(result) == len(rows), f"duplicate {key}")
    return result


def analyze(data: dict, shape: str, directory: Path) -> dict:
    require(data.get("schema_version") == 2 and "bank_operations" in data,
            "need schema 2 statistics with Bank observations")
    require(data["stop_reason"] == "Completed" and data["time_unit"] == "tick", "example did not complete")
    stop = data["at"]
    require(type(stop) is int and stop > 0, "invalid stop time")
    require(not data["summary"]["replay_attempts"] and not data["summary"]["retried_unit_attempts"],
            "unexpected retry/replay")
    expected = index(records(shape), "id")
    globals_ = index(data["globals"], "id")
    work = index(data["logical_work"], "id")
    by_global = index(data["logical_work"], "global_id")
    attempts = index(data["attempts"], "id")
    require(set(globals_) == set(by_global) == set(expected) and len(attempts) == 128,
            "incomplete or extra original work/attempts")
    for gid, source in expected.items():
        g, w = globals_[gid], by_global[gid]
        require(g["op"] == "write" and g["bytes"] == 64 and g["scheduled_release"] == g["released"] == source["at"],
                "Global release/size changed")
        require(w["channel"] == 0 and w["local_byte"] == source["address"] and w["content"] == source["content"],
                "address/content mapping mismatch")
        require(g["result"] == w["result"] == "success" and w["released"] == source["at"] and
                source["at"] <= w["finished"] == g["finished"] <= stop, "incomplete/failed logical work")

    handoffs, seen_work = [], set()
    for a in sorted(attempts.values(), key=lambda a: (a["handoff"], a["id"])):
        require(a["op"] == "write" and a["bytes"] == 64 and not a["replay"] and a["unit_attempt"] == 1 and
                len(a["work_ids"]) == 1, "unexpected command granularity/retry")
        wid = a["work_ids"][0]
        require(wid in work and wid not in seen_work, "duplicate/missing logical attempt")
        seen_work.add(wid)
        w = work[wid]
        source = expected[w["global_id"]]
        require(a["channel"] == 0 and a["local_byte"] == source["address"], "attempt address mismatch")
        times = [source["at"], a["handoff"], a["rx_arrival"], a["accepted"], a["delivered"], w["finished"]]
        require(all(type(t) is int for t in times) and times == sorted(times) and times[-1] <= stop and
                a["status"] == 0, "invalid attempt milestones/status")
        handoffs.append(dict(rank=len(handoffs) + 1, page=source["page"], global_id=source["id"],
                             address=source["address"], released=source["at"], handoff=a["handoff"],
                             rx_arrival=a["rx_arrival"], accepted=a["accepted"], delivered=a["delivered"],
                             host_wait_ps=a["handoff"] - source["at"], end_to_end_ps=w["finished"] - source["at"]))

    contexts = index(data["dlu_contexts"], "local_dlu")
    require(set(contexts) == {0, 8}, "expected one context for each independent block-start DLU")
    pages, progress = [], []
    for dlu, d in sorted(contexts.items()):
        page = "A" if dlu == 0 else "B"
        require(d["channel"] == 0 and d["received_fragments"] == 64 and not d["active"] and
                d["termination"] == "program_success", "incomplete/failed assembly context")
        require(d["opened_at"] <= d["ready_at"] <= d["program_started_at"] <= d["retired_at"] <= stop,
                "invalid context milestones")
        require(d["deadline"] == d["opened_at"] + 10_000_000, "changed assembly deadline")
        members = index(d["members"], "attempt")
        require(len(members) == 64 and all(aid in attempts and attempts[aid]["local_byte"] // 4096 == dlu
                                         and attempts[aid]["command"] == m["command"] for aid, m in members.items()),
                "Assembly membership mismatch")
        source = [r for r in expected.values() if r["page"] == page]
        page_work = [by_global[r["id"]] for r in source]
        require(max(r["at"] for r in source) <= d["ready_at"], "Ready before input availability")
        history = d["progress"]
        require(history and history == sorted(history, key=lambda r: (r["at"], r["sequence"])) and
                [r["received_fragments"] for r in history] == sorted(r["received_fragments"] for r in history) and
                history[-1]["received_fragments"] == 64, "invalid Assembly progress")
        pages.append(dict(page=page, local_dlu=dlu, first_release=min(r["at"] for r in source),
                          last_release=max(r["at"] for r in source), opened=d["opened_at"], ready=d["ready_at"],
                          program_started=d["program_started_at"], programmed=d["retired_at"],
                          logical_finished=max(w["finished"] for w in page_work),
                          assembly_ps=d["ready_at"] - d["opened_at"],
                          ready_wait_ps=d["program_started_at"] - d["ready_at"]))
        progress.extend(dict(page=page, **r) for r in history)

    ops = sorted(data["bank_operations"], key=lambda r: r["issued_at"])
    require(len(ops) == 4 and sum(o["kind"] == "erase" for o in ops) == 2 and
            sum(o["kind"] == "program" for o in ops) == 2, "unexpected media work")
    for i, op in enumerate(ops):
        require(op["channel"] == op["die"] == op["bank"] == op["page"] == 0 and op["block"] in (0, 1),
                "unexpected physical Bank/block")
        require(op["completed_at"] is not None and 0 <= op["issued_at"] <= op["completed_at"] <= stop and
                op["observed_busy_ticks"] == op["completed_at"] - op["issued_at"], "invalid Bank lifetime")
        require(i == 0 or ops[i-1]["completed_at"] <= op["issued_at"], "same-Bank overlap")
    mocs, marea = interval_curve([(a["accepted"], a["delivered"]) for a in attempts.values()], stop)
    queued, qarea = interval_curve([(r["released"], r["handoff"]) for r in handoffs], stop)
    bank, barea = interval_curve([(o["issued_at"], o["completed_at"]) for o in ops], stop)
    require(len(data["pending_channels"]) == 1, "wrong channel count")
    pc = data["pending_channels"][0]
    raw = data["pending_timeline"]
    phases = ("total", "collecting", "ready_waiting", "programming")
    require(pc["channel"] == 0 and pc["limit"] == 8 and pc["observed_from"] == 0 and pc["observed_until"] == stop,
            "wrong Pending budget/window")
    require(raw and raw[0]["at"] == 0 and raw == sorted(raw, key=lambda r: (r["at"], r["sequence"])),
            "invalid Pending timeline")
    pending = {}
    for r in raw:
        c = r["counts"]
        require(r["channel"] == 0 and 0 <= r["at"] <= stop and
                all(type(c[k]) is int and c[k] >= 0 for k in phases) and c["total"] == sum(c[k] for k in phases[1:]),
                "invalid Pending counts")
        pending[r["at"]] = c
    curves = [mocs, queued, bank, pending]
    knots = [sorted(c) for c in curves]
    resources = []
    for t in sorted(set().union(*(c.keys() for c in curves))):
        v = [c[ts[bisect_right(ts, t)-1]] for c, ts in zip(curves, knots)]
        resources.append(dict(at_ps=t, mocs=v[0], host_queued=v[1], bank_busy=v[2], **v[3]))
    for k in phases:
        require(sum(a[k] * (b["at_ps"] - a["at_ps"]) for a, b in zip(resources, resources[1:])) ==
                pc["area_context_ticks"][k], "Pending integral mismatch")
    require(mocs[stop] == queued[stop] == bank[stop] == pc["current"]["total"] == 0, "resources not drained")
    require(max(mocs.values()) <= 256 and pc["peak"]["total"] <= 8, "capacity exceeded")
    order = " ".join(f"{page}{len(list(group))}" for page, group in groupby(handoffs, key=lambda r: r["page"]))
    result = dict(stop_reason="Completed", successful_dlus=2, commands=128, handoff_order=order,
                  first_ready_ps=min(p["ready"] for p in pages), first_program_success_ps=min(p["programmed"] for p in pages),
                  simulation_end_ps=stop, logical_completion_ps=max(p["logical_finished"] for p in pages),
                  bank_busy_ps=barea, bank_utilization=barea/stop, mocs_mean=marea/stop,
                  mocs_peak_tick_boundary=max(mocs.values()), pending_peak=pc["peak"]["total"],
                  host_queue_area_fragment_ps=qarea,
                  mean_fragment_latency_ps=sum(r["end_to_end_ps"] for r in handoffs)/128)
    for p in pages:
        for key in ("ready", "program_started", "programmed", "logical_finished"):
            result[f'{p["page"]}_{key}_ps'] = p[key]
    for name, rows in (("handoffs", handoffs), ("pages", pages), ("progress", progress),
                       ("resources", resources), ("bank", ops)):
        csv_rows(directory / f"{name}.csv", rows)
    return result
