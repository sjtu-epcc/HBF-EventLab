"""Independent numerical and public-observer audits for the optional Qwen run."""
import csv
import json
import math
from collections import Counter
from pathlib import Path
from experiments.common.io import digest
from experiments.common.config import read_configuration, scalar

# Declared before measurements. No fitted/automatically relaxed tolerances.
TOLERANCES = {"hidden_kv_atol": .002, "hidden_kv_rtol": .002,
              "logits_max_abs": .005, "logits_rmse": .0005}

def require(condition, message):
    if not condition:
        raise ValueError(message)

def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))

def rows(path):
    with Path(path).open(encoding="utf-8-sig", newline="") as source:
        return list(csv.DictReader(source))

def generation(reference, result):
    require(result["success"], "run did not succeed")
    for key in ("tokens", "stop", "positions"):
        require(reference[key] == result[key], "generation differs: " + key)

def union_bytes(intervals):
    total, end = 0, 0
    for start, stop in sorted(intervals):
        require(0 <= start < stop, "invalid coverage interval")
        total += max(0, stop - max(start, end))
        end = max(end, stop)
    return total

def timeline(result, gates):
    require(result["success"] and result["tail_complete"], "unsuccessful inference/tail")
    require(len(gates) == result["gates"] and gates, "missing callback records")
    previous = result["initialization_end_tick"]
    service = 0
    for i, row in enumerate(gates):
        require(int(row["sequence"]) == i + 1, "callback sequence gap")
        compute = 0 if i == 0 else result["compute_interval_ps"]
        require(int(row["compute_ps"]) == compute, "compute interval mismatch")
        arrival, finished = int(row["arrival_tick"]), int(row["completed_tick"])
        require(arrival == previous + compute, "wall time/gap inserted into simulated clock")
        require(finished >= arrival and int(row["success"]) == 1, "callback did not complete")
        service += finished - arrival
        previous = finished
    require(result["tail_start"] == previous, "tail does not start at last callback completion")
    require(result["tail_end"] == previous + result["tail_requested_ps"], "tail duration omitted/incorrect")
    require(result["inference_end_tick"] == result["tail_end"], "inference excludes tail")
    expected_compute = (len(gates) - 1) * result["compute_interval_ps"] + result["tail_requested_ps"]
    require(result["compute_total_ps"] == expected_compute, "compute total mismatch")
    require(result["simulated_inference_ticks"] == expected_compute + service, "simulation decomposition mismatch")
    for key in ("wall_seconds", "inference_wall_seconds", "backend_wall_seconds", "executor_and_dump_wall_seconds"):
        require(math.isfinite(result[key]) and result[key] >= 0, "invalid wall time")
    return {"compute_ticks": expected_compute, "selected_memory_service_ticks": service}

def numerical(model, reference_prefix, native_prefix, hbf_prefix):
    import numpy as np  # Optional GPU experiment dependency; core tests do not import it.
    config = load(model / "config.json")
    reference = load(str(reference_prefix) + ".json")
    hidden, layers, vocab = config["hidden_size"], config["num_hidden_layers"], config["vocab_size"]
    kv = config["num_key_value_heads"] * config["head_dim"]
    per_layer = hidden + 2 * kv
    stride = layers * per_layer + hidden + vocab
    shape = (reference["positions"], stride)
    expected_size = math.prod(shape) * 4
    def matrix(prefix):
        path = Path(str(prefix) + ".bin")
        require(path.stat().st_size == expected_size, "snapshot length mismatch: " + str(prefix))
        value = np.memmap(path, dtype="<f4", mode="r", shape=shape)
        require(np.isfinite(value).all(), "nonfinite snapshot: " + str(prefix))
        return value
    expected = matrix(reference_prefix)
    output = {}
    for label, prefix in (("native", native_prefix), ("hbf", hbf_prefix)):
        result = load(str(prefix) + ".json")
        generation(reference, result)
        actual = matrix(prefix)
        delta = np.abs(actual.astype(np.float64) - expected.astype(np.float64))
        require(np.all(delta[:, :-vocab] <= TOLERANCES["hidden_kv_atol"] + TOLERANCES["hidden_kv_rtol"] * np.abs(expected[:, :-vocab])), "hidden/KV tolerance: " + label)
        logits = delta[:, -vocab:]
        maximum, rmse = float(logits.max()), float(np.sqrt(np.mean(logits ** 2)))
        require(maximum <= TOLERANCES["logits_max_abs"] and rmse <= TOLERANCES["logits_rmse"], "logits tolerance: " + label)
        layer_errors = []
        for layer in range(layers):
            start = layer * per_layer
            layer_errors.append({"layer": layer, "hidden_max_abs": float(delta[:, start:start+hidden].max()),
                "key_max_abs": float(delta[:, start+hidden:start+hidden+kv].max()),
                "value_max_abs": float(delta[:, start+hidden+kv:start+per_layer].max())})
        output[label] = {"logits_max_abs": maximum, "logits_rmse": rmse, "layers": layer_errors,
                         "tokens": result["tokens"], "positions": result["positions"]}
    require(digest(Path(str(native_prefix)+".bin")) == digest(Path(str(hbf_prefix)+".bin")), "HBF GPU output is not bitwise native GPU output")
    output["native_hbf_bitwise"] = True
    output["tolerances"] = TOLERANCES
    return output

def behavior(prefix, native_prefix, model):
    result, plan = load(str(prefix)+".json"), load(str(prefix)+".plan.json")
    gates, native = rows(str(prefix)+".gates.csv"), rows(str(native_prefix)+".gates.csv")
    timing = timeline(result, gates)
    timeline(load(str(native_prefix)+".json"), native)
    fields = ("sequence", "tensor", "offset", "bytes", "selected_offset", "selected_bytes")
    require([[r[k] for k in fields] for r in gates] == [[r[k] for k in fields] for r in native], "GPU access stream changed between modes")
    inventory = {r["name"]: r["bytes"] for r in load(model / "layout.json")}
    require(inventory[plan["tensor"]] == plan["tensor_bytes"], "plan differs from checkpoint")
    intervals, covered, parents = [], 0, []
    for row in gates:
        at, count = int(row["offset"]), int(row["bytes"])
        require(row["tensor"] in inventory and count > 0 and 0 <= at <= inventory[row["tensor"]] - count, "out-of-tensor callback")
        if row["tensor"] == plan["tensor"]:
            begin, end = max(at, plan["tensor_offset"]), min(at+count, plan["tensor_offset"]+plan["selected_bytes"])
            size = max(0, end-begin)
        else:
            size = 0
        require(int(row["selected_bytes"]) == size, "selected overlap was widened or dropped")
        if size:
            relative = begin-plan["tensor_offset"]
            require(int(row["selected_offset"]) == relative, "tensor-to-Global offset differs")
            intervals.append((relative, relative+size));covered += size;parents.append(row)
            require(int(row["globals"]) == int(row["finished"]) > 0, "parent released before all child results")
        else:
            require(int(row["parent"]) == int(row["globals"]) == 0, "unselected callback submitted to HBF")
    require(union_bytes(intervals) == plan["selected_bytes"], "selection not fully covered")
    require(covered == result["covered_access_bytes"] and len(parents) == result["selected_gates"], "coverage counters differ")
    require(result["initialized_bytes"] == plan["selected_bytes"], "incomplete initialization")
    require(result["run_status"] == "Completed" and result["host_drained"] and result["device_drained"] and result["input_closed"] and result["resident_fragments"] == 0, "run not drained")
    require(result["host_handed"] == result["host_accepted"] == result["host_responses"] and result["logical_failed"] == 0 and result["attempt_errors"] == 0, "missing responses or unexpected errors")
    defined, finished, work, work_finished = {}, {}, {}, set()
    attempts, delivered, assessed = {}, set(), set()
    sequence = 0; stream_end = None
    with Path(str(prefix)+".events.jsonl").open(encoding="utf-8") as source:
        for line in source:
            event = json.loads(line);sequence += 1
            require(event["sequence"] == sequence and stream_end is None, "event stream gap/record after end")
            kind = event["type"]
            if kind == "GlobalDefined":
                require(event["id"] not in defined, "duplicate Global");defined[event["id"]] = event
            elif kind == "GlobalFinished":
                require(event["id"] not in finished and event["success"], "duplicate/failed Global completion");finished[event["id"]] = event
            elif kind == "WorkDefined":
                require(event["id"] not in work, "duplicate logical work");work[event["id"]] = event
            elif kind == "WorkFinished":
                require(event["id"] not in work_finished and event["success"], "duplicate/failed logical work completion")
                work_finished.add(event["id"])
            elif kind == "AttemptStarted":
                require(event["id"] not in attempts, "duplicate attempt");attempts[event["id"]] = event
            elif kind in ("AttemptDelivered", "AttemptAssessed"):
                target = delivered if kind == "AttemptDelivered" else assessed
                require(event["id"] not in target, "duplicate response/assessment");target.add(event["id"])
                if kind == "AttemptDelivered":require(event["status"] == 0, "non-success Device response")
                else:require(event["success"], "Host content/result assessment failed")
            elif kind == "StreamEnd": stream_end = event
    require(stream_end and stream_end["stop_reason"] == "Completed", "missing final stream record")
    require(set(defined) == set(finished) and len(defined) == result["globals_registered"] == result["globals_finished"], "missing/excess Global completion")
    require(set(work) == work_finished, "logical work not fully terminated")
    require(set(attempts) == delivered == assessed and len(attempts) == result["host_handed"], "attempt lifecycle not exactly once")
    config = {k: scalar(v) for k, v in read_configuration(Path(str(prefix)+".effective.ini")).items()}
    channels, stripe, base = config["device.geometry.host_channels"], config["address.interleave_bytes"], config["address.global_base"]
    by_parent = {}
    for item in work.values():
        by_parent.setdefault(item["parent"], []).append(item)
    chunk, unit = plan["global_chunk_bytes"], plan["host_unit_bytes"]
    # Public bridge membership must identify the exact logical byte range, not just a count.
    registered, notifications = {}, Counter()
    for row in rows(str(prefix)+".bridge.csv"):
        parent, global_id = int(row["parent"]), int(row["global"])
        if row["action"] == "global-registered": registered.setdefault(parent, []).append(global_id)
        if row["action"] == "global-finished": notifications[global_id] += 1
    require(set(notifications) == set(defined) and all(v == 1 for v in notifications.values()), "bridge feedback not exactly once")
    membership = [g for group in registered.values() for g in group]
    require(len(membership) == len(set(membership)) and set(membership) == set(defined), "child Global has missing/duplicate parent")
    parent_specs = [(1, 0, plan["selected_bytes"], 1)] + [(int(r["parent"]), int(r["selected_offset"]), int(r["selected_bytes"]), 0) for r in parents]
    for parent, at, size, op in parent_specs:
        globals_ = registered.get(parent, [])
        require(len(globals_) == (size+chunk-1)//chunk, "Global split count differs")
        for index, global_id in enumerate(globals_):
            begin = plan["global_base"]+at+index*chunk
            count = min(chunk, size-index*chunk)
            require(defined[global_id]["bytes"] == count and defined[global_id]["op"] == op, "Global bytes/op differs")
            expected = []
            for address in range(begin, begin+count, unit):
                offset = address-base; channel = (offset//stripe) % channels
                local = (offset//(stripe*channels))*stripe + offset%stripe
                # Schema 1 exports content only for writes. Reads are checked by
                # the existing Host's expected_content/DataToken assessment.
                expected.append((channel, local, plan["content"] if op == 1 else None))
            actual = sorted((w["channel"], w["local_byte"], w["content"]) for w in by_parent.get(global_id, []))
            require(actual == sorted(expected), "logical address/content coverage differs")
        if op == 0:
            row = next(r for r in parents if int(r["parent"]) == parent)
            require(max(finished[g]["at"] for g in globals_) == int(row["completed_tick"]), "callback not at maximum final child time")
    return {**timing, "unique_selected_bytes": union_bytes(intervals), "covered_access_bytes": covered,
            "globals": len(defined), "commands": len(attempts), "input_backpressure": result["input_backpressure"], "drained": True}
