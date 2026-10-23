"""Run fourteen finite arbitration/window diagnostics; refuse output overwrite."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
if __package__ in (None, ""):
    sys.path.insert(0, str(ROOT))
from experiments.common.config import normalized_configuration
from experiments.common.io import csv_rows, digest, write_json
from experiments.common.provenance import git_info, script_hashes
from experiments.read_scheduling.workload import cases, configuration, write_workload
from experiments.read_scheduling.analyze import analyze, require


def effective(path: Path) -> dict:
    return normalized_configuration(path)


def verify_comparisons(rows: list[dict], root: Path) -> None:
    require(len(rows)==len(cases()) and all(r["analysis_status"]=="ok" for r in rows), "incomplete diagnostics")
    by_name={r["name"]:r for r in rows}
    baseline=effective(root/cases()[0]["name"]/"effective.ini")
    allowed={"execution.nand_scheduler.policy","execution.read_dispatch.wait_ps",
             "execution.read_dispatch.threshold","host.read_hint_mode"}
    for case in cases():
        config=effective(root/case["name"]/"effective.ini")
        require({k:v for k,v in config.items() if k not in allowed}==
                {k:v for k,v in baseline.items() if k not in allowed}, "resource budget changed")
        require(by_name[case["name"]]["trace_sha256"]==digest(root/"workloads"/(case["shape"]+".txt")), "trace changed")
    for wait in (0,50_000,1_000_000):
        a,b=[by_name[f"competition-w{wait}-{p}"] for p in ("fcfs","read_priority")]
        require(a["trace_sha256"]==b["trace_sha256"], "policy pair has different input")
        if wait<1_000_000:
            require(b["read_max_end_to_end_ps"]<a["read_max_end_to_end_ps"] and
                    b["program_finished_ps"]>a["program_finished_ps"], "missing read/write tradeoff")
        else:
            require(a["read_max_end_to_end_ps"]==b["read_max_end_to_end_ps"] and
                    a["program_finished_ps"]==b["program_finished_ps"], "unreleased read affected arbitration")
    a,b=[by_name[f"nonpreemptive-{p}"] for p in ("fcfs","read_priority")]
    require(a["read_max_end_to_end_ps"]==b["read_max_end_to_end_ps"] and a["program_finished_ps"]==b["program_finished_ps"],
            "already-issued program was preempted")
    require(by_name["timer"]["trace_sha256"]==by_name["threshold"]["trace_sha256"] and
            by_name["threshold"]["first_window_release_ps"]<by_name["timer"]["first_window_release_ps"],
            "threshold did not release same input earlier")


def main(argv=None) -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli",required=True,type=Path)
    parser.add_argument("--output",required=True,type=Path)
    args=parser.parse_args(argv)
    try:
        cli=args.cli.resolve(strict=True);root=args.output.resolve()
        if not cli.is_file() or root.exists():
            raise ValueError("CLI must exist and output must be a new directory")
    except (OSError,ValueError) as error:
        parser.error(str(error))
    root.mkdir(parents=True);traces=root/"workloads";traces.mkdir()
    here=Path(__file__).resolve().parent
    manifest=dict(experiment="read-scheduling-minimal-v1", created_utc=datetime.now(timezone.utc).isoformat(),
                  cli=str(cli),cli_sha256=digest(cli),git=git_info(),python=sys.version,platform=platform.platform(),
                  script_sha256=script_hashes(here),
                  cases=cases(),complete=False)
    write_json(root/"manifest.json",manifest)
    for shape in sorted({c["shape"] for c in cases()}):
        write_workload(traces/(shape+".txt"),shape)
    rows=[]
    for case in cases():
        directory=root/case["name"];directory.mkdir();trace=traces/(case["shape"]+".txt")
        config=directory/"input.ini";config.write_text(configuration(case),encoding="utf-8",newline="\n")
        command=[str(cli),str(trace),"--config",str(config),"--set",f"output.statistics_json={directory/'statistics.json'}",
                 "--set",f"output.effective_config={directory/'effective.ini'}"]
        row=dict(name=case["name"],analysis_status="harness_error",trace_sha256=digest(trace))
        start=time.perf_counter()
        try:
            with (directory/"console.log").open("wb") as log:
                process=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,timeout=120)
            row["exit_code"]=process.returncode
            require(process.returncode==0,"CLI failed; inspect console.log")
            row.update(analyze(json.loads((directory/"statistics.json").read_text(encoding="utf-8")),case,directory))
        except (OSError,ValueError,KeyError,TypeError,StopIteration,subprocess.SubprocessError) as error:
            row["error"]=f"{type(error).__name__}: {error}"
        row["process_wall_seconds"]=time.perf_counter()-start
        write_json(directory/"result.json",dict(command=command,result=row,sha256={p.name:digest(p) for p in
                   (config,directory/"statistics.json",directory/"effective.ini") if p.exists()}))
        rows.append(row);csv_rows(root/"summary.csv",rows);write_json(root/"summary.json",rows)
        print(f'{case["name"]}: {row["analysis_status"]} {row.get("measured_order",row.get("error",""))}',flush=True)
    try:
        verify_comparisons(rows,root);manifest.update(complete=True,comparison_checks="passed")
    except (ValueError,OSError) as error:
        manifest["comparison_error"]=str(error)
    manifest["harness_errors"]=sum(r["analysis_status"]!="ok" for r in rows)
    write_json(root/"manifest.json",manifest)
    return 0 if manifest["complete"] else 1


if __name__=="__main__":
    raise SystemExit(main())
