"""Analyze observations; never infer Assembly membership from acceptance notices."""
from __future__ import annotations

from bisect import bisect_right
from collections import Counter, defaultdict
from pathlib import Path

from .io_utils import csv_rows, interval_curve, ratio
from .workload import footprint, release_span

PHASES = ("total", "collecting", "ready_waiting", "programming")


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def first(values) -> int | None:
    return min((x for x in values if x is not None), default=None)


def indexed(rows: list[dict], key: str) -> dict:
    result = {r[key]: r for r in rows}
    require(len(result) == len(rows), f"duplicate {key}")
    return result


def analyze(data: dict, plan: dict, case: dict, exit_code: int, directory: Path) -> dict:
    require(data.get("schema_version") == 2 and "bank_operations" in data, "need schema 2 with Bank observations")
    stop, channels = data["at"], plan["channels"]
    require(type(stop) is int and stop >= 0 and data["time_unit"] == "tick", "invalid time unit or stop time")
    require(exit_code in (0, 2, 3, 4), "CLI input/model failure")
    require(data["stop_reason"] == {0: "Completed", 2: "Completed", 3: "Stalled", 4: "LimitReached"}[exit_code],
            "exit/stop mismatch")
    require(not data["summary"]["replay_attempts"] and not data["summary"]["retried_unit_attempts"], "unexpected retry/replay")
    expected = footprint(case["size_bytes"], case["delta_ps"], channels, case["interleave_bytes"])
    globals_ = indexed(data["globals"], "id")
    work = indexed(data["logical_work"], "id")
    by_global = indexed(data["logical_work"], "global_id")
    require(set(globals_) == set(by_global) == {r["global_id"] for r in expected}, "incomplete/extra logical footprint")
    groups = defaultdict(list)
    for r in expected:
        w, g = by_global[r["global_id"]], globals_[r["global_id"]]
        require((w["channel"], w["local_byte"], w["content"]) == (r["channel"], r["local_byte"], 1),
                "simulator mapping/content disagrees with input oracle")
        require(g["op"] == "write" and g["bytes"] == 64 and g["scheduled_release"] == r["scheduled_release"],
                "wrong source release schedule or command granularity")
        require(w["result"] in ("pending", "success", "failed"), "invalid logical outcome")
        require(w["released"] is None or w["released"] == r["scheduled_release"] <= stop, "invalid actual release")
        require(w["finished"] is None or 0 <= w["finished"] <= stop, "invalid logical finish")
        groups[r["channel"], r["local_dlu"]].append(r | w | {"scheduled_release": r["scheduled_release"]})

    attempts = indexed(data["attempts"], "id")
    by_work = {}
    for a in attempts.values():
        require(a["op"] == "write" and a["bytes"] == 64 and not a["replay"] and a["unit_attempt"] == 1,
                "expected original fragment64 writes only")
        require(len(a["work_ids"]) == 1, "attempt must reference exactly one original fragment")
        wid = a["work_ids"][0]
        require(wid in work and wid not in by_work, "unknown/repeated logical attempt")
        w = work[wid]
        require((a["channel"], a["local_byte"]) == (w["channel"], w["local_byte"]), "attempt address mismatch")
        times = [w["released"], a["handoff"], a["rx_arrival"], a["accepted"], a["delivered"]]
        require(times[0] is not None and 0 <= times[0] <= stop, "handoff without source release")
        valid_times = [x for x in times if x is not None]
        require(valid_times == sorted(valid_times) and valid_times[-1] <= stop, "invalid attempt milestones")
        require(a["delivered"] is None or a["accepted"] is not None, "delivery without acceptance")
        by_work[wid] = a

    contexts = data["dlu_contexts"]
    context_keys, context_groups, progress, context_rows = set(), defaultdict(list), [], []
    for d in contexts:
        key = (d["channel"], d["local_dlu"])
        identity = key + (d["generation"],)
        require(key in groups and identity not in context_keys, "invalid/duplicate DLU context")
        context_keys.add(identity)
        require(0 <= d["opened_at"] <= stop and d["deadline"] == d["opened_at"] + plan["assembly_timeout_ps"],
                "invalid context lifetime/deadline")
        require(d["active"] == (d["retired_at"] is None), "context activity mismatch")
        require(d["retired_at"] is None or d["opened_at"] <= d["retired_at"] <= stop, "invalid retirement")
        require(0 < d["received_fragments"] <= 64, "invalid fragment progress")
        if d["ready_at"] is not None:
            require(d["received_fragments"] == 64 and d["opened_at"] <= d["ready_at"] <= stop, "invalid Ready milestone")
            require(max(w["scheduled_release"] for w in groups[key]) <= d["ready_at"], "Ready before all input is available")
        members = set()
        for member in d["members"]:
            aid = member["attempt"]
            require(aid in attempts and aid not in members, "invalid Assembly membership")
            members.add(aid)
            a = attempts[aid]
            require((a["channel"], a["local_byte"] // 4096) == key and a["command"] == member["command"],
                    "Assembly member targets another DLU")
        require(len(members) == d["received_fragments"], "fragment64 membership/count mismatch")
        if d["termination"] == "assembly_timeout":
            require(d["retired_at"] == d["deadline"] and d["ready_at"] is None, "invalid assembly timeout")
        row = {k: v for k, v in d.items() if k not in ("members", "progress")}
        # Input-side evidence, not an assertion about the cause or policy inevitability.
        row["planned_unreleased_at_timeout"] = (sum(w["scheduled_release"] > d["deadline"] for w in groups[key])
                                                  if d["termination"] == "assembly_timeout" else None)
        row["timeout_input_class"] = ("future_planned_fragments" if row["planned_unreleased_at_timeout"] else
                                       "all_fragments_planned_by_deadline") if d["termination"] == "assembly_timeout" else ""
        context_rows.append(row)
        context_groups[key].append(d)
        progress.extend(dict(channel=key[0], local_dlu=key[1], generation=d["generation"], **v) for v in d["progress"])

    pages, fragments, queue_intervals = [], [], defaultdict(list)
    for key, ws in sorted(groups.items()):
        ds = context_groups[key]
        counts = Counter(w["result"] for w in ws)
        outcome = "pending" if counts["pending"] else "success" if counts["success"] == 64 else "failed"
        lo, hi = min(w["scheduled_release"] for w in ws), max(w["scheduled_release"] for w in ws)
        pages.append(dict(channel=key[0], local_dlu=key[1], planned_first_release=lo, planned_last_release=hi,
                          release_span_ps=hi-lo, result=outcome, successful_fragments=counts["success"],
                          failed_fragments=counts["failed"], unfinished_fragments=counts["pending"],
                          cancelled_before_release=sum(w["cancelled_before_release"] for w in ws),
                          opened_contexts=len(ds), timeout_contexts=sum(d["termination"] == "assembly_timeout" for d in ds),
                          first_opened_at=first(d["opened_at"] for d in ds), first_ready_at=first(d["ready_at"] for d in ds),
                          first_program_at=first(d["program_started_at"] for d in ds),
                          successful_program_at=first(d["retired_at"] for d in ds if d["termination"] == "program_success"),
                          finished_at=None if counts["pending"] else max(w["finished"] for w in ws)))
        for w in sorted(ws, key=lambda w: w["local_byte"]):
            a = by_work.get(w["id"])
            handed = a["handoff"] if a else None
            released, finish = w["released"], w["finished"]
            if released is not None:
                queue_intervals[key[0]].append((released, handed if a else finish))
            fragments.append(dict(global_id=w["global_id"], global_byte=(w["global_id"]-1)*64,
                                  channel=key[0], local_dlu=key[1], local_byte=w["local_byte"],
                                  scheduled_release=w["scheduled_release"], released=released, handoff=handed,
                                  rx_arrival=a["rx_arrival"] if a else None, accepted=a["accepted"] if a else None,
                                  delivered=a["delivered"] if a else None, status=a["status"] if a else None,
                                  host_wait_to_handoff_ps=handed-released if a else None,
                                  result=w["result"], finished=finish, cancelled_before_release=w["cancelled_before_release"],
                                  reason=w["reason"]))

    pending_by_ch = indexed(data["pending_channels"], "channel")
    require(set(pending_by_ch) == set(range(channels)), "missing/extra Pending channels")
    timeline_by_ch = defaultdict(list)
    for sample in data["pending_timeline"]:
        require(sample["channel"] in pending_by_ch and 0 <= sample["at"] <= stop, "invalid Pending sample")
        counts = sample["counts"]
        require(all(type(counts[k]) is int and counts[k] >= 0 for k in PHASES) and
                counts["total"] == sum(counts[k] for k in PHASES[1:]), "invalid Pending partition")
        timeline_by_ch[sample["channel"]].append(sample)

    ops_by_ch = defaultdict(list)
    tickets = set()
    for op in data["bank_operations"]:
        require(op["ticket"] not in tickets and op["channel"] in pending_by_ch and
                op["die"] == op["bank"] == 0 and op["kind"] in ("erase", "program"), "unexpected Bank operation")
        tickets.add(op["ticket"])
        end = stop if op["completed_at"] is None else op["completed_at"]
        require(0 <= op["issued_at"] <= end <= stop and op["observed_busy_ticks"] == end-op["issued_at"],
                "invalid Bank interval")
        ops_by_ch[op["channel"]].append(op)

    resources, channel_rows, bank_curves = [], [], []
    for ch in range(channels):
        pc, samples = pending_by_ch[ch], timeline_by_ch[ch]
        require(samples and samples[0]["at"] == 0 and samples == sorted(samples, key=lambda r: (r["at"], r["sequence"])),
                "unordered/missing initial Pending timeline")
        require(pc["limit"] == 16 and pc["observed_from"] == 0 and pc["observed_until"] == stop, "wrong Pending budget/window")
        pending = {r["at"]: r["counts"] for r in samples}
        for field in PHASES:
            require(max(s["counts"][field] for s in samples) == pc["peak"][field], "Pending peak mismatch")
        require(samples[-1]["counts"] == pc["current"] and pc["peak"]["total"] <= 16, "Pending current/limit mismatch")
        aas = [a for a in attempts.values() if a["channel"] == ch]
        mocs, marea = interval_curve([(a["accepted"], a["delivered"]) for a in aas if a["accepted"] is not None], stop)
        require(max(mocs.values()) <= 1024, "MOCS exceeded")
        queue, qarea = interval_curve(queue_intervals[ch], stop)
        ops = sorted(ops_by_ch[ch], key=lambda op: op["issued_at"])
        for left, right in zip(ops, ops[1:]):
            require(left["completed_at"] is not None and left["completed_at"] <= right["issued_at"], "same-Bank overlap")
        bank, barea = interval_curve([(o["issued_at"], o["completed_at"]) for o in ops], stop)
        bank_curves.append(bank)
        curves = [mocs, queue, bank, pending]
        times = [sorted(curve) for curve in curves]
        rows = []
        for t in sorted(set().union(*(curve.keys() for curve in curves))):
            values = [curve[ts[bisect_right(ts, t)-1]] for curve, ts in zip(curves, times)]
            rows.append(dict(at_ps=t, channel=ch, mocs=values[0], host_queued=values[1], bank_busy=values[2], **values[3]))
        for field in PHASES:
            area = sum(a[field]*(b["at_ps"]-a["at_ps"]) for a, b in zip(rows, rows[1:]))
            require(area == pc["area_context_ticks"][field], "Pending area mismatch")
        if exit_code in (0, 2):
            require(not (mocs[stop] or queue[stop] or bank[stop] or pc["current"]["total"]), "completed run retains resources")
        resources.extend(rows)
        chpages = [d for d in pages if d["channel"] == ch]
        chcontexts = context_groups_for_channel(contexts, ch)
        channel_rows.append(dict(channel=ch, planned_dlus=len(chpages), successful_dlus=sum(d["result"] == "success" for d in chpages),
                                 first_ready_ps=first(d["ready_at"] for d in chcontexts),
                                 first_bank_issue_ps=first(o["issued_at"] for o in ops),
                                 first_program_success_ps=first(d["retired_at"] for d in chcontexts if d["termination"] == "program_success"),
                                 mocs_peak_tick_boundary=max(mocs.values()), mocs_mean=ratio(marea, stop),
                                 mocs_full_ps=sum(a["mocs"] == 1024 and b["at_ps"]-a["at_ps"] or 0 for a, b in zip(rows, rows[1:])),
                                 host_queued_peak_tick_boundary=max(queue.values()), host_queue_area_fragment_ps=qarea,
                                 pending_peak=pc["peak"]["total"], pending_full_ps=pc["at_limit_ticks"],
                                 collecting_mean=pc["time_weighted_mean"]["collecting"],
                                 ready_waiting_mean=pc["time_weighted_mean"]["ready_waiting"],
                                 bank_busy_ps=barea, bank_utilization=ratio(barea, stop),
                                 erase_busy_ps=sum(o["observed_busy_ticks"] for o in ops if o["kind"] == "erase"),
                                 program_busy_ps=sum(o["observed_busy_ticks"] for o in ops if o["kind"] == "program")))

    outcomes = Counter(p["result"] for p in pages)
    require(exit_code != 0 or outcomes["success"] == len(pages), "success exit contains logical failures")
    require(exit_code not in (0, 2) or not outcomes["pending"], "completed run contains unfinished work")
    statuses = Counter(a["status"] for a in attempts.values() if a["delivered"] is not None)
    require(not statuses[4], "roomy first-group experiment unexpectedly exhausted Pending")
    global_times = sorted(set().union(*(curve.keys() for curve in bank_curves)))
    bank_times = [sorted(curve) for curve in bank_curves]
    busy = {t: sum(curve[ts[bisect_right(ts, t)-1]] for curve, ts in zip(bank_curves, bank_times)) for t in global_times}
    aggregate = [dict(at_ps=t, busy_banks=busy[t]) for t in global_times]
    result = dict(exit_code=exit_code, stop_reason=data["stop_reason"], simulation_end_ps=stop,
                  planned_last_release_ps=expected[-1]["scheduled_release"],
                  logical_completion_ps=None if outcomes["pending"] else max(p["finished_at"] for p in pages),
                  planned_dlus=len(pages), successful_dlus=outcomes["success"], failed_dlus=outcomes["failed"],
                  unfinished_dlus=outcomes["pending"], dlu_success_ratio=ratio(outcomes["success"], len(pages)),
                  opened_contexts=len(contexts), timeout_contexts=sum(d["termination"] == "assembly_timeout" for d in contexts),
                  analytical_release_span_ps=release_span(channels, case["interleave_bytes"], case["delta_ps"]),
                  first_ready_ps=first(d["ready_at"] for d in contexts),
                  first_successful_program_ps=first(d["retired_at"] for d in contexts if d["termination"] == "program_success"),
                  mean_bank_utilization=ratio(sum(r["bank_busy_ps"] for r in channel_rows), channels*stop),
                  mean_busy_banks=ratio(sum(r["bank_busy_ps"] for r in channel_rows), stop), peak_busy_banks=max(busy.values()),
                  all_banks_busy_ps=sum(b-a for a, b in zip(global_times, global_times[1:]) if busy[a] == channels),
                  attempts_handed=len(attempts), commands_delivered=sum(a["delivered"] is not None for a in attempts.values()),
                  unhanded_fragments=len(work)-len(attempts), cancelled_before_release=sum(w["cancelled_before_release"] for w in work.values()),
                  pending_limit_responses=statuses[4], timeout_responses=statuses[5], order_responses=statuses[6],
                  mocs_peak_per_channel=max(r["mocs_peak_tick_boundary"] for r in channel_rows),
                  mocs_full_channel_ps=sum(r["mocs_full_ps"] for r in channel_rows),
                  pending_peak_per_channel=max(r["pending_peak"] for r in channel_rows),
                  timeout_with_future_input=sum(r["timeout_input_class"] == "future_planned_fragments" for r in context_rows))
    result["timeout_context_ratio"] = ratio(result["timeout_contexts"], len(contexts))
    for name, rows, fields in (
        ("release_schedule", expected, None), ("dlus", pages, None), ("fragments", fragments, None),
        ("contexts", context_rows, None if context_rows else ["channel", "local_dlu", "generation", "termination"]),
        ("progress", progress, None if progress else ["channel", "local_dlu", "generation", "at", "received_fragments"]),
        ("pending", [dict(at_ps=s["at"], sequence=s["sequence"], channel=s["channel"], **s["counts"]) for s in data["pending_timeline"]], None),
        ("bank", data["bank_operations"], None if data["bank_operations"] else ["ticket", "channel", "kind", "issued_at", "completed_at"]),
        ("channels", channel_rows, None), ("resources", resources, None), ("bank_parallelism", aggregate, None)):
        csv_rows(directory / (name + ".csv"), rows, fields)
    return result


def context_groups_for_channel(contexts: list[dict], channel: int) -> list[dict]:
    return [d for d in contexts if d["channel"] == channel]
