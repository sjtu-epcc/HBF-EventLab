"""Synthetic scheduler/window controls: fairness, real outcomes and corruption guards."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
import unittest
import shutil
import uuid
from contextlib import contextmanager

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from experiments.read_scheduling.workload import START, cases, configuration, records, write_workload
from experiments.read_scheduling.analyze import analyze
from experiments.read_scheduling.run import verify_comparisons

@contextmanager
def temporary(parent):
    parent=parent.resolve();path=parent/("read-scheduling-"+uuid.uuid4().hex);path.mkdir()
    try:
        yield path
    finally:
        if path.is_symlink() or path.resolve().parent!=parent:
            raise RuntimeError("cleanup escaped test directory")
        shutil.rmtree(path)


class UnitTests(unittest.TestCase):
    def test_matrix(self):
        all_cases=cases()
        self.assertEqual(len(all_cases),14)
        self.assertEqual(len({c["name"] for c in all_cases}),14)
        self.assertEqual({c["policy"] for c in all_cases},{"fcfs","read_priority"})

    def test_preload_and_independent_write(self):
        rows=records("competition")
        self.assertEqual(rows[0]["bytes"],4*4096)
        self.assertEqual(rows[1]["address"],32768)
        self.assertLess(rows[2]["address"],rows[0]["bytes"])
        self.assertEqual(rows[1]["at"],START)

    def test_no_choice_changes_only_read_arrival(self):
        a,b=records("competition"),records("nonpreemptive")
        self.assertEqual(a[:2],b[:2])
        self.assertEqual({k:v for k,v in a[2].items() if k!="at"},{k:v for k,v in b[2].items() if k!="at"})
        self.assertEqual(b[2]["at"]-a[2]["at"],1_000_000)

    def test_cold_and_shared_page_shapes(self):
        self.assertEqual([r["address"] for r in records("four")[1:]],[0,4096,8192,12288])
        self.assertEqual({r["address"]//4096 for r in records("same_page")[1:]},{0})

    def test_isolated_policy_treatment(self):
        a,b=cases()[:2]
        self.assertEqual(configuration(a).replace("policy = fcfs\n[execution.ucie_scheduler]",
                                                "policy = read_priority\n[execution.ucie_scheduler]"),configuration(b))
        self.assertIn("read_hint_mode = all_regular",configuration(a))
        self.assertIn("send_policy = fcfs",configuration(b))

    def test_trace_exclusive(self):
        with temporary(Path.cwd()) as root:
            file=root/"trace.txt";write_workload(file,"competition")
            self.assertIn("expect=1 after=1",file.read_text())
            with self.assertRaises(FileExistsError):write_workload(file,"competition")

    def test_bad_input(self):
        with self.assertRaises(ValueError):records("unknown")
        bad=cases()[0];bad["policy"]="unknown"
        with self.assertRaises(ValueError):configuration(bad)


def check(ok,why):
    if not ok:raise AssertionError(why)


def integration(cli,parent):
    parent.mkdir(parents=True,exist_ok=True)
    with temporary(parent) as root:
        runner=ROOT/"experiments/read_scheduling/run.py"
        def scan(target):
            p=subprocess.run([sys.executable,str(runner),"--cli",str(cli),"--output",str(target)],
                             cwd=ROOT,text=True,capture_output=True,timeout=150)
            check(p.returncode==0,p.stdout+p.stderr)
            check(json.loads((target/"manifest.json").read_text())["complete"],"complete comparisons")
            return json.loads((target/"summary.json").read_text())
        first,repeat=root/"first",root/"repeat"
        rows,again=scan(first),scan(repeat)
        stable=lambda rs:[{k:v for k,v in r.items() if k!="process_wall_seconds"} for r in rs]
        check(stable(rows)==stable(again),"deterministic results")
        for case in cases():
            a=json.loads((first/case["name"]/"statistics.json").read_text())
            b=json.loads((repeat/case["name"]/"statistics.json").read_text())
            check(a==b,"all event observations deterministic")
        p=subprocess.run([sys.executable,str(runner),"--cli",str(cli),"--output",str(first)],capture_output=True,timeout=30)
        check(p.returncode!=0,"refuse output overwrite")
        bad=copy.deepcopy(rows);bad[0]["trace_sha256"]="changed"
        try:verify_comparisons(bad,first)
        except ValueError:pass
        else:raise AssertionError("different trace accepted")
        case=next(c for c in cases() if c["name"]=="timer")
        data=json.loads((first/"timer/statistics.json").read_text())
        variants=[]
        bad=copy.deepcopy(data);bad["attempts"].append(bad["attempts"][0]);variants.append(bad)
        bad=copy.deepcopy(data);bad["read_window_releases"][0]["at"]+=1;variants.append(bad)
        bad=copy.deepcopy(data);bad["read_paths"][0]["service"]="cache";variants.append(bad)
        bad=copy.deepcopy(data);bad["bank_operations"][1]["issued_at"]=0;variants.append(bad)
        bad=copy.deepcopy(data);bad["globals"][0]["finished"]=START;variants.append(bad)
        bad=copy.deepcopy(data);bad["stop_reason"]="LimitReached";variants.append(bad)
        for bad in variants:
            try:analyze(bad,case,first/"timer")
            except (ValueError,KeyError):pass
            else:raise AssertionError("corrupt observations accepted")
        print("PASS read scheduling: 14 cases, deterministic repeat, fair inputs, trigger/Cache/nonpreemptive controls and corruption guards")


if __name__=="__main__":
    parser=argparse.ArgumentParser();parser.add_argument("--cli",type=Path);parser.add_argument("--output",type=Path)
    args=parser.parse_args()
    if args.cli:
        if not args.output:parser.error("--cli requires --output")
        integration(args.cli.resolve(strict=True),args.output.resolve())
    else:unittest.main(argv=[sys.argv[0]])
