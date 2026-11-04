# S 系列：真实 CUDA + 当前 EventLab 后端

本目录从 ZeePgm/RealGPUAcess_v0 的 `minexp` 提交
`d6cb5016aed9ec72f3bc36f4be114bcd6cb8bdcb` 选择性导入。
[来源清单](upstream-origin.json) 保留导入时各文件哈希；没有导入旧 vendored 后端。
当前主要入口是 [integrated.py](integrated.py)，历史 S0–S9 分阶段脚本保留用于理解原实验。

## 当前实验是什么

- CUDA 真正执行 Qwen3-0.6B F16，使用原生 F16 KV；每个算子前后回调均经过实际同步。
- 仅 M2 选定权重范围经过公共 `TensorAccessBridge`、`SimulationSession`、Host、Device。
  权重实际仍在 GPU；HBF 校验的是符号内容，不是物理字节搬运。
- G2 为纯记录零等待重放。G3 的模拟时间 = **同一固定计算表 + 实际后端返回的模拟等待**。
- 此集成默认复用组员 RTX 5070 的合格固定表。它是明确声明的外部输入，
  **不是本地 GPU 新采样的计算时间**。本地 CUPTI 只验证边界、覆盖、依赖因果及真实 CUDA 执行。
- H0 未经硬件校准；微小模拟差异不证明真实硬件可观测的性能收益。
- 固定轨迹不允许丢段、截掉 EOS 后缀、重选时长或替换模型哈希来通过验收。
  本地活动/空段边界若不同，应停止并另行设计本机 G1 采样，不能放松覆盖检查。

## 原始包：获取与只读核验

原始包在上游 `artifacts/minexp`，包含 102 个分片、索引和标准库恢复脚本。
已发布的模型 GGUF 本体不在其中；必须另供同哈希权重。

```powershell
# 在固定提交的上游审查仓库中执行；目标须为新目录。
python artifacts/minexp/restore.py --destination <records-root>
# 以下命令在 HBF-EventLab 根目录执行。
python integrations/real_gpu/experiments/gpu_s/verify_records.py `
  --bundle <upstream>/artifacts/minexp --records <records-root> --output <new-audit-dir>
```

[verify_records.py](verify_records.py) 检查所有恢复字节、包/表/采样身份，
重新运行历史数组数值及 KV 发布核验，并在新目录做 G2 重放。
它**不执行新 GPU 或新后端**；历史通过与新运行通过必须分开报告。
曾缺失的 10,825 个快照由组员重新生成后与原索引逐字节匹配；包内保留恢复过程，
不应把新的墙钟或 API 时间戳说成历史采样时间戳。

## 构建

先按前端集成指南构建模型依赖，指定完整 CUDA/cuBLAS 和 CUPTI SDK，关闭 CUDA graphs。
`BASE_BUILD` 必须来自本前端的 `adapters/llama_cpp/integrated` CMake 入口。
独立 cuBLAS SDK 可使用 `CMAKE_INCLUDE_PATH`、`CUDA_cublas_LIBRARY` 和
`CUDA_cublasLt_LIBRARY` 指定；运行环境从真实缓存路径解析 SDK DLL，不更改系统 PATH。

```powershell
$frontend = 'integrations/real_gpu'
$base = '<absolute-model-build>'
$build = '<absolute-S-build>'
cmake -S "$frontend/experiments/gpu_s" -B $build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DBASE_BUILD=$base `
  -DHBFLAB_EVENTLAB_SOURCE='<absolute-HBF-EventLab-root>' `
  -DCUDAToolkit_ROOT='<CUDA-root>' -DHBFLAB_CUPTI_ROOT='<CUPTI-root>'
cmake --build $build --target gpu_s_native gpu_s_s8 gpu_s_bridge --parallel 2
```

实验 CMake 直接从当前后端源构建，不链接旧 vendor 后端。
CUPTI Kernel9/Kernel10 由实际头文件探测选择；执行前检查 CUPTI 运行库 API 版本一致。
其他历史阶段目标仍可显式构建，例如 `gpu_s_segmented`、`gpu_s_reference`。

## 身份封存与执行

在代码、配置及全部构建产物稳定后，分别封存基础构建与实验构建。
已有 seal 不允许覆盖；源或二进制更改后须在明确的新构建/证据上下文中重新建立身份。
现有基础 seal 不包含本实验目录，所以必须使用附加的实验 seal。

```powershell
python "$frontend/tools/integrated_build_identity.py" seal --build-dir $base
python "$frontend/experiments/gpu_s/build_identity.py" seal --build $build
python "$frontend/experiments/gpu_s/integrated.py" `
  --build $build --records <records-root> --model <exact-F16.gguf> `
  --output <new-run-dir> --timing-repeats 2
```

Python 需要 NumPy、PyTorch CPU 及 vendor `gguf-py` 的依赖。不得使用 `-O` 或
`PYTHONOPTIMIZE`（会禁用历史审计中的断言）。当前 fixture 固定 8 步上限，非通用模型执行器。
新入口显式接收构建、模型、原始记录和输出目录，不使用组员机器的绝对路径。
原 `s6.py` / `s7.py` / `s8.py` 等 `run` 入口仍保存历史研究批次选择，**不是本集成复现入口**。
旧 S0/S1 若单独使用，可通过 `GPU_S_BUILD`、`GPU_S_BASE_BUILD` 选择构建。

新入口依次核验：

1. 固定表/模型/prompt/协议身份与封存构建；新 G2 零等待重放。
2. 本地原生 CUDA、完整逻辑事件和活动覆盖；有效 KV 发布；独立 CPU 数学参照。
3. G3-full：所有选中依赖成功后才发射消费者；GPU 原生与 G3 数值逐位一致；CPU分类判据。
4. 至少一个 timing 变体：不导出快照，但完整 logits、模拟状态、token 时间与 full 相同。
5. 初始化 program-fail 与首次消费者限额负例：不得继续发射依赖消费者或输出 token。
6. 父完成=max子完成、普通写初始化、Host内容核验、关闭/排空、逐token整数计时守恒。

最终以新目录中的 `result.json` 和完整审计为准，不能只看退出码或套用历史 pass。
负例中的限额停止预期保留未排空状态，不能报告为正常完成排空。

## 指标与边界测试

[token_contracts.py](token_contracts.py) 将 EOS-ready 与真正输出分开：
TTFT=T1−T0；TPOT=(TN−T1)/(N−1)。N=0 无 TTFT/TPOT；N=1 无 TPOT。
EOS 的计算/等待仍守恒，但不计作输出。可变输出数必须有对应完整冻结表。

```powershell
python -B -m unittest discover -s "$frontend/experiments/gpu_s/tests" -v
```

这些 CPU synthetic 测试不等于真实 GPU EOS 或新工作负载的实验验收。
