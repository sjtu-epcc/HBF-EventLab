"""Six simultaneous 64-byte reads. Run: python host_order_control.py CLI OUTPUT_DIR."""
import json
import subprocess
import sys
from pathlib import Path

from read_order import ReadOrder

HERE = Path(__file__).resolve().parent
RELEASE = 10_000_000_000


def workload(stride):
    lines = ["HBF_EVENTLAB_WORKLOAD 1"]
    for i in range(3):
        lines.append(f"WRITE {i+1} {i*stride} 4096 at={(i+1)*1_000_000_000} content=1")
    for i in range(6):
        address = i % 3 * stride + i // 3 * 64
        lines.append(f"READ {4+i} {address} 64 at={RELEASE} expect=1")
    return "\n".join(lines) + "\n"


def run(cli, root, layout, policy):
    case = root / (layout + "-" + policy)
    case.mkdir(parents=True, exist_ok=True)
    trace = root / (layout + ".txt")
    stats = case / "stats.json"
    log = case / "run.log"
    with log.open("w") as out:
        subprocess.run([cli, trace, "--config", HERE/"config/gpt2-eventlab-device.ini",
                        "--set", "host.read_hint_mode=all_regular", "--set",
                        "host.send_policy=" + policy, "--trace", "--stats-json", stats],
                       stdout=out, stderr=subprocess.STDOUT, check=True)
    s = json.loads(stats.read_text())
    assert s["stop_reason"] == "Completed"
    order = ReadOrder()
    for line in log.read_text().splitlines():
        fields = line.split()
        if fields:
            order.consume(fields[0], fields[1:])
    obs = order.finish()
    reads = [a for a in s["attempts"] if a["op"] == "read"]
    assert len(reads) == len(obs["handoffs"]) == 6
    assert all(a["host_success"] and a["status"] == 0 for a in reads)
    def parent(a):
        h = obs["handoffs"][a["id"]]
        assert h["accepted_at"] == a["accepted"] and h["at"] == a["handoff"]
        return next(o["parent"] for o in h["options"] if o["unit"] == h["selected"])
    accepted = [parent(a) for a in sorted(reads, key=lambda a:a["accepted"])]
    assert accepted == ([4,5,6,7,8,9] if policy == "fcfs" else [4,7,5,8,6,9])
    assert [len(h["options"]) for h in obs["handoffs"].values()] == [6,5,4,3,2,1]
    ops = [o for o in s["bank_operations"] if o["kind"] == "sense"]
    banks = {(o["channel"],o["die"],o["bank"]) for o in ops}
    assert len(banks) == (1 if layout == "same" else 3)
    (case/"order.json").write_text(json.dumps(obs,indent=2))
    return dict(layout=layout,policy=policy,accepted=accepted,sense=len(ops),
                cache=sum(p["service"]=="cache" for p in s["read_paths"]),
                last_return_ps=max(a["delivered"] for a in reads)-RELEASE,
                wait_count=len(obs["wait_intervals"]),
                different_page_wait_ps=sum(w["duration"] for w in obs["wait_intervals"]
                                           if w["page"]!=w["prior_page"]))


if __name__ == "__main__":
    cli, root = map(Path,sys.argv[1:])
    root.mkdir(parents=True,exist_ok=True)
    results = []
    for layout,stride in [("same",33554432),("spread",65536)]:
        (root/(layout+".txt")).write_text(workload(stride))
        for policy in ("fcfs","same_read_page"):
            results.append(run(cli.resolve(),root,layout,policy))
    (root/"summary.json").write_text(json.dumps(results,indent=2))
    print(json.dumps(results,indent=2))
