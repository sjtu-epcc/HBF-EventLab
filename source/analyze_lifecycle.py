import csv
import json
import statistics
import sys
from collections import Counter

path = sys.argv[1]
max_id = int(sys.argv[2]) if len(sys.argv) > 2 else None
min_id = int(sys.argv[3]) if len(sys.argv) > 3 else None
by_id = {}
events = Counter()
latencies = []
pages = Counter()
first_tick = None
last_tick = None
for row in csv.DictReader(open(path, newline="")):
    event = row["event"]
    key = int(row["id"])
    if (max_id is not None and key > max_id) or (min_id is not None and key < min_id):
        continue
    events[event] += 1
    tick = int(row["tick_ps"])
    first_tick = tick if first_tick is None else min(first_tick, tick)
    last_tick = tick if last_tick is None else max(last_tick, tick)
    if event == "INGRESS":
        by_id[key] = tick
        pages[int(row["address"]) // 4096] += 1
    elif event == "COMPLETED" and key in by_id:
        latencies.append(tick - by_id[key])

latencies.sort()
def quantile(q):
    if not latencies:
        return None
    return round(latencies[int((len(latencies)-1)*q)] / 1e6, 4)

print(json.dumps({
    "events": events,
    "trace_span_ms": round((last_tick-first_tick)/1e9, 6),
    "matched_latency_count": len(latencies),
    "latency_us_p50": quantile(.5),
    "latency_us_p90": quantile(.9),
    "latency_us_p99": quantile(.99),
    "latency_us_max": quantile(1),
    "uncompleted_ingress_at_timeout": events["INGRESS"]-len(latencies),
    "unique_ingress_pages": len(pages),
    "max_sector_requests_to_one_page": max(pages.values(), default=0),
}, indent=2))
