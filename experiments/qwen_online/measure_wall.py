"""Bounded, sequential wall-time reproduction of the teammate's 8 MiB case.

All runs use the same checkpoint/prompt and one generated token. The current
Host keeps its contracts; the external old Host is only an unmodified baseline.
Timeout samples are censored measurements, never successful completions.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
from experiments.common.io import digest, write_json
from experiments.common.provenance import git_info, script_hashes
from experiments.qwen_online.run import write_config
from experiments.qwen_online.verify import load, rows, require, numerical, behavior, timeline


def measured(command, prefix, seconds):
    start = time.perf_counter()
    with Path(str(prefix)+".process.log").open("w", encoding="utf-8") as log:
        try:
            result = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT,
                                    timeout=seconds, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            status = "completed" if result.returncode == 0 else "failed"
            code = result.returncode
        except subprocess.TimeoutExpired:
            status, code = "wall_budget_reached", None
    sample = {"label": prefix.name, "command": [str(x) for x in command], "status": status,
              "returncode": code, "wall_seconds": time.perf_counter()-start, "wall_budget_seconds": seconds}
    result_file = Path(str(prefix)+".json")
    if result_file.exists():sample["result"] = load(result_file)
    progress = Path(str(prefix)+".progress.csv")
    if progress.exists():
        records = rows(progress)
        sample["last_flushed_progress"] = records[-1] if records else None
        sample["progress_records"] = len(records)
    write_json(Path(str(prefix)+".measurement.json"), sample)
    return sample


def summary(output, samples, provenance):
    write_json(output/"measurements.json", {"samples": samples, "provenance": provenance})
    lines = ["# Qwen 8MiB 规模与墙钟复现", "",
             "同一机器串行运行的诊断样本；不是硬件时延或稳定性能排名。墙钟预算超限的样本保留最后进展，不算成功。", "",
             "| 样本 | 状态 | 进程墙钟(s) | 初始化(s) | 推理期间后端推进(s) | 推理总墙钟(s) |",
             "|---|---|---:|---:|---:|---:|"]
    for s in samples:
        r=s.get("result",{})
        def value(key):return f"{r[key]:.6f}" if key in r else "—"
        lines.append(f"| {s['label']} | {s['status']} | {s['wall_seconds']:.3f} | {value('initialization_wall_seconds')} | {value('inference_backend_wall_seconds')} | {value('inference_wall_seconds') if 'inference_wall_seconds' in r else value('seconds')} |")
    lines += ["", "组员 detailed 模式为第一层 Q 投影的完整8MiB，未逐命令模拟全部权重；aggregate 模式只按校准成本计账。",
              "原路径：StreamingHost、两页父批次、child_window=4、4KiB写初始化、每页独立version。当前路径：原Host、fragment64写、64份驻留预算、既有block内容身份，无child_window。",
              "当前尺寸梯度采用相同设备几何（1 channel/1 die/2 Bank、4096 pages/block）、MOCS=256、Pending=4、RX/TX各1项；运行限额显式放宽以容纳本次命令数量。策略/身份语义并非等价，差值不能直接归因于某一个函数。",
              "统计full/off只控制详细事件及桥接日志；两者都保留小体积逐GPU回调日志、每128个Global的进展和末尾汇总。微型对照验证开关不改变数值、模拟时刻和业务计数。",
              "原程序的seconds不含初始化及模型加载；本脚本另计整个进程墙钟，不能混用。原生GPU与所有成功样本检查tokens、位置数和完整数值快照。",
              "最新进展、逐回调后端耗时、输入预算、实际配置、进程命令及二进制/输入哈希见同目录JSON、CSV。超限样本仅给出完成时间下界，不能外推为全量权重耗时。", ""]
    (output/"report.md").write_text("\n".join(lines), encoding="utf-8")


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--program",type=Path,required=True)
    p.add_argument("--teammate-program",type=Path,required=True)
    p.add_argument("--model",type=Path,required=True)
    p.add_argument("--reference-prefix",type=Path,required=True)
    p.add_argument("--output",type=Path,required=True)
    p.add_argument("--wall-budget",type=float,default=300)
    p.add_argument("--sizes",type=int,nargs="+",default=[131072,1048576,8388608])
    a=p.parse_args()
    require(a.wall_budget>0 and all(n>0 and n%8192==0 and n<=8388608 for n in a.sizes),"invalid wall budget/sizes")
    output=a.output.resolve();require(not output.exists(),"use a new output directory");output.mkdir(parents=True)
    model=a.model.resolve();samples=[]
    manifest=load(model/"download-manifest.json")
    require(manifest["revision"]=="c1899de289a04d12100db370d81485cdf75e47ca","unexpected checkpoint")
    require(digest(model/"weights-fp32.bin")==manifest["fp32_sha256"],"weight hash changed")
    provenance={"repository":git_info(),"scripts":script_hashes(Path(__file__).parent),
                "program_sha256":digest(a.program),"teammate_program_sha256":digest(a.teammate_program),
                "model_revision":manifest["revision"],"fp32_sha256":manifest["fp32_sha256"],
                "prompt_sha256":digest(model/"prompt.ids"),"reference_sha256":digest(Path(str(a.reference_prefix)+".bin"))}
    # Same static geometry/buffer budgets as the reviewed teammate experiment.
    config={"device.geometry.host_channels":"1","device.geometry.dies_per_channel":"1","device.geometry.banks_per_die":"2",
            "device.geometry.pages_per_block":"4096","device.geometry.blocks_per_bank":"4","device.geometry.spare_blocks_per_bank":"1",
            "device.capabilities.max_pending_write_dlus":"4","device.capabilities.mocs_supported":"256",
            "device.selected.mocs_selected":"256","device.selected.zone_size_dlus":"8192",
            "execution.buffers.ingress_entries":"1","execution.buffers.ingress_payload_bytes":"4096",
            "execution.buffers.egress_entries":"1","execution.buffers.egress_payload_bytes":"64",
            "host.max_resident_fragments":"64","host.max_logical_fragments":"4194304","host.retained_fragments":"131072",
            "host.send_policy":"fcfs","host.write_mode":"fragment64","run.total_events":"100000000"}
    config_path=output/"backend.ini";write_config(config_path,config)
    def run(label,cmd):
        print("START",label,flush=True)
        sample=measured(cmd,output/label,a.wall_budget);samples.append(sample)
        summary(output,samples,provenance)
        print(label,sample["status"],round(sample["wall_seconds"],3),flush=True)
        require(sample["status"]!="failed","experiment failed: "+label)
        return sample
    common=[a.program.resolve(),"--model",model,"--max-new",1,"--compute-ps",1000,"--tail-ps",5000,"--resident",64,"--config",config_path]
    native=run("current-native",common+["--mode","native","--bytes",8192,"--detail","off","--output-prefix",output/"current-native"])
    require(native["status"]=="completed","native baseline incomplete")
    expected=native["result"]
    reference=load(Path(str(a.reference_prefix)+".json"))
    for k in ("positions","tokens","stop"):require(expected[k]==reference[k],"CPU/native generation differs")
    for mode in ("native","timing-selected"):
        label="teammate-"+mode
        sample=run(label,[a.teammate_program.resolve(),mode,model,output/label,1,0,0,0])
        if sample["status"]=="completed":
            for k in ("positions","tokens","stop"):require(sample["result"][k]==expected[k],"teammate generation differs")
            require(digest(output/(label+".bin"))==digest(output/"current-native.bin"),"teammate/native numerical output differs")
            sample["numerical_bitwise_native"]=True
    smoke={}
    for size,detail in [(8192,"full"),(8192,"off")]+[(n,"off") for n in a.sizes]:
        label=f"current-{size}-{detail}"
        sample=run(label,common+["--mode","hbf","--bytes",size,"--detail",detail,"--output-prefix",output/label])
        if sample["status"]!="completed":continue
        r=sample["result"]
        require(r["success"] and r["host_drained"] and r["device_drained"] and not r["resident_fragments"],"not successfully drained")
        require(r["initialized_bytes"]==size and r["covered_access_bytes"]==size*r["positions"],"incomplete selected coverage")
        require(r["globals_registered"]==r["globals_finished"]==(size//4096)*(r["positions"]+1),"Global count differs")
        require(r["host_handed"]==r["host_responses"]==(size//64)*(r["positions"]+1) and not r["attempt_errors"],"command count/errors differ")
        sample["numerical"]=numerical(model,a.reference_prefix.resolve(),output/"current-native",output/label)
        sample["timeline"]=timeline(r,rows(output/(label+".gates.csv")))
        if size==8192:
            smoke[detail]=r
            if detail=="full":sample["behavior"]=behavior(output/label,output/"current-native",model)
        summary(output,samples,provenance)
    require(set(smoke)=={"full","off"},"statistics comparison incomplete")
    for key in ("tokens","positions","final_tick","globals_finished","host_handed","programs","senses","cache_hits","events_executed"):
        require(smoke["full"][key]==smoke["off"][key],"statistics altered behavior: "+key)
    provenance["detail_toggle_equivalent"]=True
    summary(output,samples,provenance)
    print("MEASURED",output/"report.md",flush=True)

if __name__=="__main__":main()
