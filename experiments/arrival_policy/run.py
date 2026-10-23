"""Run the four-cell minimal arrival-shape x Host-policy example."""
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

from experiments.common.config import normalized_configuration, OUTPUT_PATHS
from experiments.common.io import csv_rows, digest, write_json
from experiments.common.provenance import git_info, script_hashes
from experiments.arrival_policy.analyze import analyze
from experiments.arrival_policy.workload import POLICIES, SHAPES, configuration, records, write_workload


def run_case(cli: Path, directory: Path, trace: Path, shape: str, policy: str) -> dict:
    directory.mkdir(parents=True)
    config = directory / "input.ini"
    config.write_text(configuration(policy), encoding="utf-8", newline="\n")
    command = [str(cli), str(trace), "--config", str(config), "--set",
               f'output.statistics_json={directory / "statistics.json"}', "--set",
               f'output.effective_config={directory / "effective.ini"}']
    result = dict(shape=shape, policy=policy, analysis_status="harness_error", trace_sha256=digest(trace))
    start = time.perf_counter()
    try:
        with (directory / "console.log").open("wb") as log:
            p = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=120)
        result["process_wall_seconds"] = time.perf_counter()-start
        result["exit_code"] = p.returncode
        if p.returncode != 0:
            raise ValueError(f"minimal successful example failed (exit={p.returncode}); inspect console.log")
        data = json.loads((directory / "statistics.json").read_text(encoding="utf-8"))
        result.update(analyze(data, shape, directory))
        result["analysis_status"] = "ok"
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        result["error"] = f"{type(error).__name__}: {error}"
    result.setdefault("process_wall_seconds", time.perf_counter()-start)
    write_json(directory / "result.json", dict(command=command, result=result,
               sha256={p.name: digest(p) for p in (config, directory / "statistics.json", directory / "effective.ini") if p.exists()}))
    return result


def normalized_effective(path: Path) -> dict:
    return normalized_configuration(path, OUTPUT_PATHS | {"host.send_policy"})


def verify_comparisons(rows: list[dict], root: Path) -> None:
    """Validate treatment fairness and the expected minimal policy contrast."""
    if len(rows) != 4 or any(r["analysis_status"] != "ok" for r in rows):
        raise ValueError("four successful examples are required")
    for shape in SHAPES:
        pair = {r["policy"]: r for r in rows if r["shape"] == shape}
        if set(pair) != set(POLICIES) or pair["fcfs"]["trace_sha256"] != pair["fill_open"]["trace_sha256"]:
            raise ValueError("policy pair does not share exactly the same trace")
        configs = [normalized_effective(root / shape / policy / "effective.ini") for policy in POLICIES]
        if configs[0] != configs[1]:
            raise ValueError("effective configurations differ beyond policy/output paths")
    pairs = {(r["shape"], r["policy"]): r for r in rows}
    fc, fill = pairs["batched", "fcfs"], pairs["batched", "fill_open"]
    if fc["handoff_order"] != "A32 B32 A32 B32" or fill["handoff_order"] != "A64 B64":
        raise ValueError("batched example did not expose the expected policy choice")
    if not fill["A_ready_ps"] < fc["A_ready_ps"]:
        raise ValueError("fill_open did not assemble A earlier")
    ignored = {"policy", "process_wall_seconds"}
    s1, s2 = pairs["sparse", "fcfs"], pairs["sparse", "fill_open"]
    if {k:v for k,v in s1.items() if k not in ignored} != {k:v for k,v in s2.items() if k not in ignored}:
        raise ValueError("sparse no-choice control differs between policies")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="new directory; refuses overwrite")
    args = parser.parse_args(argv)
    try:
        cli, root = args.cli.resolve(strict=True), args.output.resolve()
        if not cli.is_file() or root.exists():
            raise ValueError("CLI must be a file and output directory must not exist")
    except (OSError, ValueError) as error:
        parser.error(str(error))
    root.mkdir(parents=True)
    traces = root / "workloads"
    traces.mkdir()
    manifest = dict(experiment="arrival-policy-minimal-v1", created_utc=datetime.now(timezone.utc).isoformat(),
                    cli=str(cli), cli_sha256=digest(cli), git=git_info(), python=sys.version, platform=platform.platform(),
                    script_sha256=script_hashes(HERE),
                    complete=False)
    write_json(root / "manifest.json", manifest)
    rows = []
    for shape in SHAPES:
        trace = traces / f"{shape}.txt"
        write_workload(trace, shape)
        csv_rows(traces / f"{shape}.csv", records(shape))
        for policy in POLICIES:
            row = run_case(cli, root / shape / policy, trace, shape, policy)
            rows.append(row)
            write_json(root / "summary.json", rows)
            csv_rows(root / "summary.csv", rows)
            print(f'{shape}/{policy}: {row["analysis_status"]} {row.get("handoff_order", row.get("error", ""))}', flush=True)
    try:
        verify_comparisons(rows, root)
        manifest.update(complete=True, comparison_checks="passed")
    except (ValueError, OSError) as error:
        manifest["comparison_error"] = str(error)
    manifest["harness_errors"] = sum(r["analysis_status"] != "ok" for r in rows)
    write_json(root / "manifest.json", manifest)
    if not manifest["complete"]:
        print(manifest.get("comparison_error", "incomplete example"), file=sys.stderr)
    return 0 if manifest["complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
