"""Run fixed-FCFS, multi-channel interleave x release-interval experiments."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))

from experiments.interleave_arrival.analyze import analyze
from experiments.interleave_arrival.io_utils import csv_rows, digest, write_json
from experiments.interleave_arrival.plan import case_name, cases, configuration, validate_plan
from experiments.interleave_arrival.workload import write_workload


def trace_name(case: dict) -> str:
    return f'n{case["size_bytes"]}-d{case["delta_ps"]}.txt'


def git_info() -> dict:
    try:
        def git(*args: str) -> str:
            return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True,
                                           stderr=subprocess.DEVNULL, timeout=10).strip()
        if Path(git("rev-parse", "--show-toplevel")).resolve() != ROOT:
            return {"commit": None, "branch": None, "dirty": None}
        return dict(commit=git("rev-parse", "HEAD"), branch=git("branch", "--show-current"), dirty=bool(git("status", "--porcelain")))
    except (OSError, subprocess.SubprocessError):
        return {"commit": None, "branch": None, "dirty": None}


def run_case(cli: Path, directory: Path, trace: Path, plan: dict, case: dict, timeout: float) -> dict:
    directory.mkdir(parents=True)  # Refuse to overwrite any earlier case.
    config = directory / "input.ini"
    config.write_text(configuration(plan, case), encoding="utf-8", newline="\n")
    args = [str(cli), str(trace), "--config", str(config), "--set",
            f'output.statistics_json={directory / "statistics.json"}', "--set",
            f'output.effective_config={directory / "effective.ini"}']
    row = dict(case=case_name(case), channels=plan["channels"], **case, analysis_status="harness_error")
    metadata = dict(command=args, case=case, trace_sha256=digest(trace), input_sha256=digest(config))
    start = time.perf_counter()
    try:
        with (directory / "console.log").open("wb") as log:
            process = subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        row["process_wall_seconds"] = time.perf_counter() - start
        row["exit_code"] = process.returncode
        if process.returncode not in (0, 2, 3, 4):
            raise ValueError(f"CLI input/model failure ({process.returncode}); inspect console.log")
        stats = json.loads((directory / "statistics.json").read_text(encoding="utf-8"))
        row.update(analyze(stats, plan, case, process.returncode, directory))
        row["analysis_status"] = "ok"
        metadata.update(statistics_sha256=digest(directory / "statistics.json"), effective_sha256=digest(directory / "effective.ini"))
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        row.update(analysis_status="harness_error", error=f"{type(error).__name__}: {error}")
        row.setdefault("process_wall_seconds", time.perf_counter()-start)
    write_json(directory / "result.json", metadata | {"result": row})
    return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="new experiment directory")
    parser.add_argument("--plan", type=Path, default=HERE / "plans" / "round1.json")
    parser.add_argument("--timeout-seconds", type=float, default=120)
    parser.add_argument("--max-cases", type=int, help="explicitly incomplete prefix scan")
    args = parser.parse_args(argv)
    try:
        cli, output = args.cli.resolve(strict=True), args.output.resolve()
        if not cli.is_file() or output.exists():
            raise ValueError("CLI must be a file; output directory must not exist")
        if not 0 < args.timeout_seconds < float("inf") or (args.max_cases is not None and args.max_cases <= 0):
            raise ValueError("run limits must be positive and finite")
        plan = validate_plan(json.loads(args.plan.read_text(encoding="utf-8-sig")))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    matrix = cases(plan)
    selected = matrix if args.max_cases is None else matrix[:args.max_cases]
    output.mkdir(parents=True)
    traces = output / "workloads"
    traces.mkdir()
    for case in selected:
        path = traces / trace_name(case)
        if not path.exists():
            write_workload(path, case["size_bytes"], case["delta_ps"])
    manifest = dict(experiment="interleave-arrival-v1", created_utc=datetime.now(timezone.utc).isoformat(),
                    plan=plan, total_cases=len(matrix), selected_cases=len(selected), scan_complete=False,
                    cli=str(cli), cli_sha256=digest(cli), git=git_info(), python=sys.version, platform=platform.platform(),
                    script_sha256={p.name: digest(p) for p in sorted(HERE.glob("*.py"))},
                    workload_sha256={p.name: digest(p) for p in sorted(traces.glob("*.txt"))})
    write_json(output / "manifest.json", manifest)
    rows = []
    for index, case in enumerate(selected, 1):
        row = run_case(cli, output / "cases" / case_name(case), traces / trace_name(case), plan, case, args.timeout_seconds)
        rows.append(row)
        csv_rows(output / "summary.csv", rows)
        write_json(output / "summary.json", rows)
        print(f'[{index}/{len(selected)}] {row["case"]}: {row.get("stop_reason", row["analysis_status"])} '
              f'success={row.get("successful_dlus", "?")}/{case["size_bytes"]//4096} '
              f'timeouts={row.get("timeout_contexts", "?")}', flush=True)
    errors = sum(r["analysis_status"] != "ok" for r in rows)
    manifest.update(finished_cases=len(rows), harness_errors=errors, scan_complete=len(rows) == len(matrix) and not errors)
    write_json(output / "manifest.json", manifest)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
