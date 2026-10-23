"""Validate observed choices, not a second implementation of window/assembly state."""
from pathlib import Path
from experiments.common.io import csv_rows
from experiments.common.config import RunConfiguration, load_run_configuration
from experiments.common.model import PAGE_BYTES, FRAGMENT_BYTES
from .workload import START, records


def require(ok: bool, why: str) -> None:
    if not ok:
        raise ValueError(why)


def analyze(data: dict, case: dict, directory: Path | None = None, *, config: RunConfiguration | None = None) -> dict:
    if config is None:
        if directory is None:
            raise ValueError("analysis requires the run's effective configuration")
        config = load_run_configuration(directory)
    require(config.channels == 1, "expected single-channel diagnostic")
    require(config.text("execution.nand_scheduler.policy") == case["policy"] and
            config.integer("execution.read_dispatch.wait_ps") == case["wait"] and
            config.integer("execution.read_dispatch.threshold") == case["threshold"], "treatment differs from actual configuration")
    require(data.get("schema_version")==2 and "read_paths" in data and "read_window_releases" in data,
            "need schema 2 with read-path/window observations")
    require(data["stop_reason"]=="Completed", "run did not drain")
    source = {r["id"]:r for r in records(case["shape"], config.pages_per_block)}
    globals_ = {r["id"]:r for r in data["globals"]}
    work = {r["id"]:r for r in data["logical_work"]}
    attempts = {r["id"]:r for r in data["attempts"]}
    require(len(attempts)==len(data["attempts"]) and len(work)==len(data["logical_work"]), "duplicate identities")
    require(set(globals_)==set(source) and len(globals_)==len(data["globals"]), "unexpected Globals")
    require(len(work)==sum(r["bytes"]//FRAGMENT_BYTES for r in source.values()) and len(attempts)==len(work), "missing/duplicate work")
    require(all(g["result"]=="success" and g["remaining"]==0 for g in globals_.values()) and
            all(w["result"]=="success" for w in work.values()), "logical failure")
    require(globals_[1]["finished"]<START, "warmup overlaps measurement")
    require(not data["summary"]["replay_attempts"] and not data["summary"]["retried_unit_attempts"], "unexpected retries")
    covered = set()
    for a in attempts.values():
        require(a["bytes"]==FRAGMENT_BYTES and len(a["work_ids"])==1 and a["unit_attempt"]==1 and not a["replay"], "unexpected attempt shape")
        wid = a["work_ids"][0]
        require(wid in work and wid not in covered, "duplicate logical attempt")
        covered.add(wid)
        w = work[wid]; g = globals_[w["global_id"]]; r = source[g["id"]]
        require(a["channel"]==0 and a["local_byte"]==w["local_byte"] and
                r["address"]<=a["local_byte"]<r["address"]+r["bytes"], "address mismatch")
        require(g["released"]==g["scheduled_release"]==r["at"], "input timing changed")
        times = [r["at"],a["handoff"],a["rx_arrival"],a["accepted"],a["delivered"],w["finished"]]
        require(all(type(t) is int for t in times) and times==sorted(times) and
                a["status"]==0 and a["host_success"] is True, "invalid time/status/data")
    ops = sorted(data["bank_operations"], key=lambda o:(o["issued_at"],o["ticket"]))
    require(all(o["completed_at"] is not None and o["issued_at"]<o["completed_at"] for o in ops), "unfinished Bank operation")
    require(all(a["completed_at"]<=b["issued_at"] for a,b in zip(ops,ops[1:])), "single Bank overlap")
    measured = [o for o in ops if o["issued_at"]>=START]
    bank = {o["ticket"]:o for o in ops}
    paths = {p["attempt"]:p for p in data["read_paths"]}
    require(len(paths)==len(data["read_paths"]), "duplicate read path")
    require(set(paths)=={a["id"] for a in attempts.values() if a["op"]=="read"}, "missing read observation")
    rows = []
    for aid,p in paths.items():
        a = attempts[aid]; w=work[a["work_ids"][0]]; r=source[w["global_id"]]
        require(p["service"] in ("sense","cache") and a["accepted"]<=p["service_at"]<=a["delivered"], "missing read service")
        if p["window_entered_at"] is not None:
            require(a["accepted"]==p["window_entered_at"]<=p["window_left_at"]<=p["service_at"], "bad window lifecycle")
        if p["service"]=="sense":
            op=bank[p["bank_ticket"]]
            require(op["kind"]=="sense" and op["issued_at"]==p["service_at"] and p["release_reason"] is not None,
                    "sense before release / wrong ticket")
        else:
            require(p["bank_ticket"] is None, "cache hit consumed Bank")
        rows.append(dict(global_id=w["global_id"], released=r["at"], accepted=a["accepted"],
                         delivered=a["delivered"], end_to_end_ps=w["finished"]-r["at"],
                         accepted_response_ps=a["delivered"]-a["accepted"], **p))
    batches = data["read_window_releases"]
    members = set()
    for b in batches:
        require(b["channel"]==0 and b["members"], "empty/foreign window release")
        for aid in b["members"]:
            require(aid not in members and paths[aid]["window_left_at"]==b["at"] and
                    paths[aid]["release_reason"]==b["reason"], "duplicate/mismatched release")
            members.add(aid)
        if b["reason"]=="timer":
            require(b["at"]==b["deadline"]==b["first_arrival"]+case["wait"], "timer not at fixed first-arrival deadline")
        if b["reason"]=="threshold":
            require(len(b["members"])==case["threshold"] and
                    b["at"]==max(paths[aid]["window_entered_at"] for aid in b["members"]), "wrong count-trigger boundary")
    require(members=={aid for aid,p in paths.items() if p["release_reason"] is not None}, "missing batch membership")
    shape = case["shape"]
    if shape in ("competition","nonpreemptive"):
        require(len(measured)==3 and measured[0]["kind"]=="erase", "expected one erase/program/sense")
        want_read_first = shape=="competition" and case["wait"]<1_000_000 and case["policy"]=="read_priority"
        require([o["kind"] for o in measured]==(["erase","sense","program"] if want_read_first else ["erase","program","sense"]),
                "unexpected arbitration order")
        dlu = next(d for d in data["dlu_contexts"] if d["local_dlu"]==config.pages_per_block)
        require(dlu["ready_at"]<measured[0]["completed_at"], "write not ready at decision boundary")
        release=rows[0]["window_left_at"]
        if shape=="competition" and case["wait"]<1_000_000:
            require(release<measured[0]["completed_at"], "read not eligible during erase")
        else:
            program=next(o for o in measured if o["kind"]=="program")
            require(program["issued_at"]<release<program["completed_at"], "negative control misses active program")
    else:
        reason = "batch" if case["hints"]=="tail_batch" else "threshold" if case["threshold"] and len(rows)>=case["threshold"] else "timer"
        require(len(batches)==1 and batches[0]["reason"]==reason, "wrong release trigger / stale timer fired")
        if shape=="same_page":
            require(len(measured)==1 and sum(p["service"]=="cache" for p in paths.values())==2, "miss-to-hit transition missing")
            require(sum(p["release_reason"] is None and p["window_left_at"] is not None for p in paths.values())==1,
                    "unreleased cached member was not removed")
        else:
            require(len(measured)==len(rows) and all(p["service"]=="sense" for p in paths.values()), "cold-read control hit cache")
    if directory:
        csv_rows(directory/"reads.csv", rows)
        csv_rows(directory/"bank_operations.csv", measured)
        csv_rows(directory/"window_releases.csv", batches)
    program=next((o for o in measured if o["kind"]=="program"),None)
    return dict(name=case["name"], policy=case["policy"], shape=shape, wait_ps=case["wait"], threshold=case["threshold"],
                read_count=len(rows), senses=sum(p["service"]=="sense" for p in paths.values()),
                cache_hits=sum(p["service"]=="cache" for p in paths.values()),
                read_mean_end_to_end_ps=sum(r["end_to_end_ps"] for r in rows)/len(rows),
                read_max_end_to_end_ps=max(r["end_to_end_ps"] for r in rows),
                first_window_release_ps=min(b["at"] for b in batches),
                program_started_ps=program["issued_at"] if program else None,
                program_finished_ps=program["completed_at"] if program else None,
                measured_order=" ".join(o["kind"] for o in measured),
                measurement_finished_ps=max(g["finished"] for g in globals_.values()),
                analysis_status="ok")
