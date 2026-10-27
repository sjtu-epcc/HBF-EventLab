"""Run real CPU/native-GPU/HBF-GPU validation. No parallel Host or timing proxy."""
import argparse
import json
import importlib.metadata
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:sys.path.insert(0, str(ROOT))
from experiments.common.io import digest, write_json
from experiments.common.provenance import git_info, script_hashes
from experiments.common.config import read_configuration
from experiments.qwen_online.verify import load, rows, require, numerical, behavior

def run(command, log, expected=0):
    start = time.perf_counter()
    with log.open("w", encoding="utf-8") as out:
        result = subprocess.run([str(v) for v in command], stdout=out, stderr=subprocess.STDOUT)
    if result.returncode != expected:
        raise RuntimeError(f"exit {result.returncode}, expected {expected}: {command[0]}; see {log}")
    return time.perf_counter()-start

def write_config(path, values):
    sections = {}
    for key, value in values.items():
        section, key = key.rsplit(".", 1);sections.setdefault(section, {})[key] = value
    path.write_text("\n".join("["+section+"]\n"+"\n".join(f"{k} = {v}" for k,v in data.items()) for section,data in sections.items())+"\n", encoding="utf-8")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--executor-source", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--tensor", default="model.layers.0.self_attn.q_proj.weight")
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--bytes", type=int, default=8192, help="0 selects the remaining tensor")
    parser.add_argument("--max-new", type=int, default=1)
    parser.add_argument("--compute-ps", type=int, default=1000)
    parser.add_argument("--tail-ps", type=int, default=5000)
    parser.add_argument("--resident", type=int, default=64)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--failure-checks", action="store_true", help="also test real initialization failure and a mid-inference run limit")
    args = parser.parse_args()
    model, source, output = args.model.resolve(), args.executor_source.resolve(), args.output.resolve()
    require(not output.exists() or not any(output.iterdir()), "output must be a new/empty directory")
    output.mkdir(parents=True, exist_ok=True)
    manifest = load(model / "download-manifest.json")
    require(manifest["repo"] == "Qwen/Qwen3-0.6B", "only the reviewed Qwen3-0.6B executor is supported")
    require(manifest["revision"] == "c1899de289a04d12100db370d81485cdf75e47ca", "checkpoint revision differs from reviewed executor")
    checkpoint = next((entry for entry in manifest["files"] if entry["name"] == "model.safetensors"), None)
    require(checkpoint and checkpoint["sha256"] == "f47f71177f32bcd101b7573ec9171e6a57f4f4d31148d38e382306f42996874b", "missing/different pinned checkpoint")
    for entry in manifest["files"]:
        require(digest(model/entry["name"]) == entry["sha256"], "checkpoint hash differs: "+entry["name"])
    require(digest(model/"weights-fp32.bin") == manifest["fp32_sha256"], "FP32 weights changed")
    for name, expected in manifest["prepared_files"].items():
        require(digest(model/name) == expected, "prepared input changed: "+name)
    timings = {}
    timings["reference_wall_seconds"] = run([sys.executable, source/"scripts/reference.py", "--model", model, "--output", output, "--counts", args.max_new, "--threads", 8], output/"reference.log")
    common = [args.program.resolve(), "--model", model, "--tensor", args.tensor, "--offset", args.offset, "--bytes", args.bytes,
              "--max-new", args.max_new, "--compute-ps", args.compute_ps, "--tail-ps", args.tail_ps, "--resident", args.resident, "--device", args.device]
    if args.config:common += ["--config", args.config.resolve()]
    for mode in ("native", "hbf"):
        print("Running", mode, flush=True)
        timings[mode+"_process_wall_seconds"] = run(common+["--mode", mode, "--output-prefix", output/mode], output/(mode+".log"))
    numeric = numerical(model, output/f"reference-{args.max_new}", output/"native", output/"hbf")
    audit = behavior(output/"hbf", output/"native", model)
    failures = {}
    if args.failure_checks:
        values = read_configuration(output/"hbf.effective.ini")
        # Persistent real Device program failure must stop before loading/using the GPU weights.
        bad = dict(values);bad["faults.persistent_page"] = "0";bad["faults.fail_sequence"] = "[]"
        failure_config = output/"initialization-failure.ini";write_config(failure_config, bad)
        base = common[:]
        if "--config" in base:
            index = base.index("--config");del base[index:index+2]
        run(base+["--mode", "hbf", "--config", failure_config, "--output-prefix", output/"initialization-failure"], output/"initialization-failure.log", 2)
        failure = load(output/"initialization-failure.json")
        require(not failure["success"] and failure["positions"] == 0 and failure["gates"] == 0 and failure["host_drained"] and failure["device_drained"] and failure["attempt_errors"] > 0, "initialization failure was ignored/not drained")
        require(not failure["inference_started"] and failure["simulated_inference_ticks"] == 0, "failed initialization counted as inference")
        failures["initialization_error_abort_and_drain"] = True
        # A genuine simulation ceiling inside the first selected read must not become success.
        first = next(r for r in rows(output/"hbf.gates.csv") if int(r["selected_bytes"]) > 0)
        bad = dict(values);bad["run.max_tick_ps"] = str(int(first["arrival_tick"])+1)
        failure_config = output/"read-limit.ini";write_config(failure_config, bad)
        run(base+["--mode", "hbf", "--config", failure_config, "--output-prefix", output/"read-limit"], output/"read-limit.log", 2)
        failure = load(output/"read-limit.json")
        require(not failure["success"] and failure["run_status"] == "LimitReached" and failure["positions"] == 0 and not failure["tokens"] and not failure["tail_complete"], "failed dependency continued GPU execution")
        require(not (failure["host_drained"] and failure["device_drained"]), "limited in-flight state was falsely reported as drained")
        failures["mid_read_limit_aborts_executor_preserves_unfinished"] = True
    source_hashes = {name: digest(source/name) for name in ("src/qwen.cu", "include/hbf_gpu/qwen.hpp", "scripts/prepare.py", "scripts/reference.py")}
    own_sources = [ROOT/"CMakeLists.txt"]
    for directory in ("src", "include", "cmake", "experiments/qwen_online"):
        own_sources += [p for p in (ROOT/directory).rglob("*") if p.suffix in (".cpp", ".hpp", ".cmake") or p.name == "CMakeLists.txt"]
    environment = {"python": sys.version, "platform": platform.platform(),
                   "packages": {name: importlib.metadata.version(name) for name in ("torch", "transformers", "numpy", "safetensors", "huggingface-hub", "tokenizers")}}
    artifacts = {p.name:digest(p) for p in sorted(output.iterdir()) if p.is_file()}
    report = {"pass": True, "scope": "selected tensor subrange; real bytes remain resident in GPU executor", "calibrated_performance": False,
              "numerical": numeric, "behavior": audit, "failure_checks": failures, "process_wall": timings,
              "native": load(output/"native.json"), "hbf": load(output/"hbf.json"), "selection": load(output/"hbf.plan.json"),
              "provenance": {"repository": git_info(), "environment": environment,
                  "compiled_sources": {p.relative_to(ROOT).as_posix():digest(p) for p in sorted(own_sources)},
                  "scripts": script_hashes(Path(__file__).parent), "external_executor": source_hashes,
                  "binary_sha256": digest(args.program.resolve()), "model_revision": manifest["revision"], "fp32_sha256": manifest["fp32_sha256"], "artifacts": artifacts}}
    write_json(output/"verification.json", report)
    text = f"""# Qwen 在线接入验证结果

- 三方数值核验：通过。CPU 为独立 Transformers FP32 eager；HBF 接入 GPU 与原生 GPU 快照逐字节一致。
- 选择：`{args.tensor}`，tensor 偏移 {report['selection']['tensor_offset']}，长度 {report['selection']['selected_bytes']} B。
- 真实权重始终由 CUDA 执行器持有；这里验证符号内容、依赖与模拟时序，不表示 NAND 向 GPU 传输了真实权重。
- 行为核验：{audit['globals']} 个 Global、{audit['commands']} 次命令尝试全部终结；Host/Device 排空。
- 输入容量背压：{audit['input_backpressure']} 次；未添加独立 child_window。
- 模拟计算：{audit['compute_ticks']} ps；选中访问服务：{audit['selected_memory_service_ticks']} ps。
- HBF 推理区间模拟时间：{report['hbf']['simulated_inference_ticks']} ps，另列初始化完成时刻 {report['hbf']['initialization_end_tick']} ps。
- 尾段：{report['hbf']['tail_requested_ps']} ps，已计入；固定计算与介质时延未经性能校准。
- HBF 运行程序墙钟：{report['hbf']['wall_seconds']:.6f} s；其中后端推进 {report['hbf']['backend_wall_seconds']:.6f} s。
- 执行器/数据快照墙钟包含 CUDA 同步、CPU 编排和文件输出，不是纯 GPU kernel 时间。
- 异常验收：{json.dumps(failures, ensure_ascii=False)}。

逐层/KV/logits 误差、阈值、配置、覆盖和文件哈希见 `verification.json`。运行限制保留未完成状态，不能报告为正常排空。
"""
    (output/"report.md").write_text(text, encoding="utf-8")
    print("PASS three-way numerical and online behavioral verification:", output/"report.md")

if __name__ == "__main__":
    main()
