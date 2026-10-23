"""Run a deterministic capacity sweep. Simulator failures are data, harness failures are errors."""
from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))
from experiments.common.io import csv_rows, digest, write_json
from experiments.common.provenance import git_info, script_hashes
from experiments.capacity.analyze import analyze
from experiments.capacity.plan import case_name, cases, configuration, validate_plan
from experiments.capacity.workload import write_workload


def run_case(cli: Path, case_dir: Path, trace: Path, plan: dict, case: dict, timeout: float) -> dict:
    case_dir.mkdir(parents=True)  # Never overwrite an earlier experiment.
    config = case_dir / "input.ini"
    config.write_text(configuration(plan, case), encoding="utf-8", newline="\n")
    statistics = case_dir / "statistics.json"
    effective = case_dir / "effective.ini"
    args = [str(cli), str(trace), "--config", str(config), "--set", f"output.statistics_json={statistics}",
            "--set", f"output.effective_config={effective}"]
    row = {"case": case_name(case), **case, "analysis_status": "harness_error"}
    metadata = {"command": args, "trace_sha256": digest(trace), "input_sha256": digest(config), "case": case}
    started = time.perf_counter()
    try:
        with (case_dir / "console.log").open("wb") as log:
            result = subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        row["process_wall_seconds"] = time.perf_counter() - started
        row["exit_code"] = result.returncode
        if result.returncode not in (0, 2, 3, 4):
            raise ValueError(f"simulator returned {result.returncode}; inspect console.log")
        data = json.loads(statistics.read_text(encoding="utf-8"))
        row.update(analyze(data, plan["dlus"], case["mocs_selected"], result.returncode, case_dir))
        row["analysis_status"] = "ok"
        metadata["statistics_sha256"] = digest(statistics)
        metadata["effective_sha256"] = digest(effective)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        row["analysis_status"] = "harness_error"
        row["process_wall_seconds"] = time.perf_counter() - started
        row["error"] = f"{type(error).__name__}: {error}"
    metadata["result"] = row
    write_json(case_dir / "result.json", metadata)
    return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True, help="built hbf_eventlab_cli executable")
    parser.add_argument("--plan", type=Path, default=HERE / "plans" / "smoke.json")
    parser.add_argument("--output", type=Path, required=True, help="new directory, e.g. out/capacity-smoke")
    parser.add_argument("--timeout-seconds", type=float, default=120, help="wall-clock process timeout per case")
    parser.add_argument("--max-cases", type=int, help="run only a prefix; the run is explicitly marked incomplete")
    args = parser.parse_args(argv)
    try:
        cli, out = args.cli.resolve(strict=True), args.output.resolve()
        if not cli.is_file() or out.exists():
            raise ValueError("CLI must be a file and output must be a new directory")
        if not 0 < args.timeout_seconds < float("inf") or (args.max_cases is not None and args.max_cases <= 0):
            raise ValueError("limits must be positive and finite")
        plan = validate_plan(json.loads(args.plan.read_text(encoding="utf-8-sig")))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    matrix = cases(plan)
    selected = matrix if args.max_cases is None else matrix[:args.max_cases]
    out.mkdir(parents=True)
    traces = out / "workloads"
    traces.mkdir()
    for order in plan["orders"]:
        write_workload(traces / f"{order}.txt", plan["dlus"], order)
    manifest = {"experiment": "capacity-interaction-v1", "created_utc": datetime.now(timezone.utc).isoformat(),
                "plan": plan, "total_cases": len(matrix), "selected_cases": len(selected),
                "scan_complete": False, "cli": str(cli), "cli_sha256": digest(cli),
                "git": git_info(), "python": sys.version, "platform": platform.platform(),
                "script_sha256": script_hashes(HERE),
                "workload_sha256": {p.name: digest(p) for p in sorted(traces.glob("*.txt"))}}
    write_json(out / "manifest.json", manifest)
    rows = []
    for index, case in enumerate(selected, 1):
        row = run_case(cli, out / "cases" / case_name(case), traces / f'{case["order"]}.txt', plan, case, args.timeout_seconds)
        rows.append(row)
        csv_rows(out / "summary.csv", rows)
        write_json(out / "summary.json", rows)
        print(f'[{index}/{len(selected)}] {row["case"]}: {row.get("stop_reason", row["analysis_status"])} '
              f'success={row.get("successful_dlus", "?")}/{plan["dlus"]} '
              f'timeouts={row.get("timeout_contexts", "?")} 0x4={row.get("pending_limit_responses", "?")}', flush=True)
    errors = sum(r["analysis_status"] != "ok" for r in rows)
    manifest.update(finished_cases=len(rows), harness_errors=errors, scan_complete=len(rows) == len(matrix) and not errors)
    write_json(out / "manifest.json", manifest)
    # Expected workload failures and censored runs remain inspectable data, not harness failures.
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
