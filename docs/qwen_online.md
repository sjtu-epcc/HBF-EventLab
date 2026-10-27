# Qwen 在线接入与三方验证

此可选实验复用外部 Qwen3-0.6B CUDA 执行器，通过 `TensorAccessBridge` 调用 HBF EventLab 原有 Host。它验证真实推理能否受后端的容量、时间和完成反馈驱动。核心库及默认构建不依赖 CUDA、PyTorch 或模型权重。

## 接入范围

```text
Qwen CUDA 执行器：真实权重、真实计算、访存回调
  → 实验适配器：筛选 tensor 子范围、换算偏移、显式计算时间
  → TensorAccessBridge：Global 分组、容量等待、父访存完成
  → SimulationSession → 原 Host → 原 Device
  → Global 最终结果 → 回调返回 → CUDA 继续依赖计算
```

执行器来源为 [Small_case](https://github.com/ZeePgm/Small_case)，本次验证使用提交 `3d8a58ba1c321f41edc3280acda70388964a39c9`。构建只引用该仓库的 `src/qwen.cu` 和 `include/hbf_gpu/qwen.hpp`，准备和 CPU 参照分别调用其 `scripts/prepare.py`、`scripts/reference.py`。不构建其旧在线运行器、StreamingHost 或后端。源码通过显式外部路径提供，未复制到本仓库；若后续要分发这些外部源码，需先确认相应授权。

执行器先将真实 FP32 权重加载到 GPU；HBF 仅初始化所选范围的符号内容并约束访问完成。**这不是把真实权重从 NAND 传到 GPU，也不是完整的 GPU–HBM–HBF 数据迁移或性能模型。** 默认只选择第一层 q_proj 权重的 8KiB 子范围。所有其他真实计算仍执行，其访存回调保留在日志中，但未选择的范围不产生 HBF 命令。

## 环境与构建

本次验证环境：Windows x64、VS 2022/MSVC 19.43、CUDA Toolkit 12.9.1、RTX 4070 Laptop GPU（计算能力 8.9）、Python 3.12。执行器需要约 2.5GB 空闲显存，建议至少留出 3GiB；模型准备另需数 GB 内存和磁盘。其他平台和 GPU 尚未完成本次实测。

以下 PowerShell 示例从项目根目录运行。外部源码、模型和虚拟环境放在仓库之外；按本机位置替换变量。

```powershell
$executor = '../Small_case'
$model = '../models/Qwen3-0.6B'
py -3.12 -m venv ../qwen-env
$python = '../qwen-env/Scripts/python.exe'
& $python -m pip install torch==2.7.1 --index-url https://download.pytorch.org/whl/cpu
& $python -m pip install -r experiments/qwen_online/requirements.txt
& $python "$executor/scripts/prepare.py" --model $model

cmake -S . -B build-qwen -G 'Visual Studio 17 2022' -A x64 -T cuda=12.9 `
  -DHBF_EVENTLAB_QWEN_EXPERIMENT=ON `
  "-DHBF_EVENTLAB_QWEN_SOURCE_DIR=$executor" `
  -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-qwen --config Release --target hbf_eventlab_qwen
```

选用已安装的 VS/CUDA 组合；可选 CUDA 目标要求 CMake 3.24+，默认核心构建仍为 CMake 3.20+。Python 的 CPU PyTorch 用于准备权重和参照计算，CUDA 推理由 C++/nvcc 编译的执行器完成，无需 CUDA 版 PyTorch。Windows 构建会将对应 CUDA runtime DLL 放到可执行文件旁。

准备脚本固定模型修订 `c1899de289a04d12100db370d81485cdf75e47ca`，生成 `weights-fp32.bin`、tensor 布局、运行配置、prompt 和 `download-manifest.json`。已有检查点可使用 `--local-only`。运行脚本校验固定修订、原始检查点 SHA-256、FP32 和准备文件哈希，不自动下载或修改权重。

## 运行

```powershell
& $python experiments/qwen_online/run.py `
  --program build-qwen/experiments/qwen_online/Release/hbf_eventlab_qwen.exe `
  --executor-source $executor --model $model `
  --output build-qwen/validation --failure-checks
```

输出目录必须不存在或为空。脚本依次执行独立 Transformers CPU 参照、原生 CUDA、接入 HBF 的 CUDA，然后检查数值、公开事件和地址覆盖。没有 GPU 或显存不足时明确失败，不退回 CPU 冒充 GPU 结果。GPU 实验需要显式运行，不计入默认 CTest；当前默认测试范围和数量见[验证说明](verification.md)。

| 参数 | 默认值与含义 |
|---|---|
| `--tensor` | `model.layers.0.self_attn.q_proj.weight`；必须为执行器实际使用的权重。 |
| `--offset` / `--bytes` | tensor 内字节偏移 0、长度 8192；偏移 64B 对齐、长度为整 4KiB 页，不能越界；长度 0 选择剩余范围。 |
| `--max-new` | 1；生成 token 数上限，prompt 加该数不超过执行器的 128 位置容量。 |
| `--compute-ps` / `--tail-ps` | 1000 / 5000；相邻访存回调之间、最后一次回调之后的固定模拟计算时间；允许 0。 |
| `--resident` | 64；传给原 Host 的活动逻辑片段预算，按 64B 计费。 |
| `--config` | 可选后端 INI，沿用统一配置解析和校验；几何不足或无法前进时明确失败。 |
| `--device` | 0；CUDA 设备编号。 |
| `--failure-checks` | 附加初始化持续 program fail、选中读途中运行限制两个异常验收。 |

直接调用 `hbf_eventlab_qwen` 时还需 `--mode native|hbf --model DIR --output-prefix PATH`，可指定 `--chunk`（默认4096），它只控制 Global 子范围大小。当前 Python 三方脚本固定使用默认 chunk。`--resident` 是脚本显式覆盖项，即使给了 INI 也会覆盖其中同名 Host 预算。

直接调用可使用`--detail full|off`（默认full）。off不挂接详细统计sink，也不写逐Global桥接日志；仍保留数值快照、逐执行器回调、每128个Global及父访问结束时的轻量进展、最终业务计数。该开关用于区分记录开销，不能把off样本声称为通过了完整JSONL审计。尺寸与程序墙钟复现入口见[墙钟测量](qwen_wall_time.md)。

默认 HBF 为 1 channel、1 die、2 banks、8 pages/block，按所选范围派生足够的逻辑 block 数并保留备用块；默认 `fragment64`、FCFS。注册区从配置的 Global base 起算，tensor 偏移单独记录，不把 GPU 指针当 HBF 地址。提供 INI 时使用其中几何，不自动扩容。多 channel 初始化要求完整覆盖 local DLU；小预算与交织导致补片不能登记时，仍可能超时或停滞。

## 时间、初始化与完成契约

初始化通过 `bridge.initialize()` 的普通 Global 写进行，原 Host 完成拆分、MOCS、Pending、erase/program 和恢复。它不会直接改写 NAND 状态；同一注册区使用不可变 content=1，读通过 Host 的 `expected_content` 和 DataToken 校验。

一次执行器访存只把与选中范围的交集提交给 HBF。适配器等待该父访存所有已登记 Global 的最终逻辑结果；容量不足保留未登记后缀，根据原 Host 容量通知继续。未添加 `child_window`。读失败时回调抛出错误，执行器不能继续依赖计算。

首次回调的计算间隔为 0；之后**每个**执行器回调（包括未选中的范围）前推进固定 `compute_ps`。计算区间仍执行后端事件。最后一次访存后通过 `advance_compute(tail_ps)` 处理计算尾段，再关闭输入并检查排空；不靠虚构末尾读请求推进时间。零时延保持同 Tick 因果续体，正时延使用排他时间边界；运行限制不变成正常暂停。

```text
推理模拟时间 = 所有显式计算间隔 + 选中访存服务时间 + 显式尾段
```

初始化完成时刻另列。真实模型加载、CUDA 计算、模拟器推进和快照输出的墙钟时间单独记录；它们不进入上述模拟时间公式。执行器/快照墙钟包含同步、CPU 编排和输出，并非纯 kernel 时间。固定计算和默认 NAND/链路参数未经校准，不能据此声称 Qwen 的真实吞吐。

## 输出与核验

| 文件 | 内容 |
|---|---|
| `*.plan.json`、`*.effective.ini` | 选中 tensor、偏移、长度、Global 基址、content、容量与实际后端配置。 |
| `*.gates.csv` | 每个真实访存回调、精确交集、计算间隔、产生/完成时刻及父访问结果。 |
| `*.progress.csv` | 轻量进展：阶段、已终结Global数、实际命令计数、驻留数和程序墙钟；中断记录仅为最后已刷新进展。 |
| `*.bridge.csv`、`*.events.jsonl` | 父访问与 Global 归属、容量反馈、原有统一统计事件流。 |
| `reference-N.bin`、`native.bin`、`hbf.bin` | 每位置逐层 hidden/K/V、最终 hidden 和 logits 的 FP32 快照。 |
| `native.json`、`hbf.json` | tokens、停止原因、模拟时间、初始化/推理/后端推进墙钟分项、Host/Device 排空、命令及事件计数。 |
| `verification.json`、`report.md` | 三方误差、固定阈值、行为结论、源码/模型/可执行文件/结果哈希及简要报告。 |

数值阈值在运行前声明：hidden/KV 的 `atol=0.002, rtol=0.002`；logits 最大绝对误差不超过0.005、RMSE不超过0.0005。生成 token、位置数、结束原因必须与 CPU 相同；原生 GPU 与 HBF GPU 的完整快照必须逐字节相同，不按结果自动放宽阈值。

行为审计只读公开 observer/统计输出：检查整个选中范围被覆盖、每个 Global 拆分后的 local 地址正确、内容评估成功、命令/Global/父访问完成恰好一次、父完成时刻等于最后一个子结果、输入容量反馈，以及正常结束时 Host/Device 排空。既有统计 schema 的 Read `WorkDefined.content` 为空，实际读内容校验仍由 Host 完成，不能将空字段误判为没有内容身份。

异常验收中，初始化持续失败应在使用 GPU 权重前终止并排空；读途中触及 `LimitReached` 应停止执行器并保留未完成诊断，不能伪报成功或排空。

## 已验证结果与边界

2026-10-26 的默认 8KiB 范围测试通过：23 token prompt、1个生成 token（785）、24个执行位置；CPU/GPU logits 最大绝对误差约 `6.53e-5`、RMSE约 `7.25e-6`。原生 GPU 与 HBF GPU 快照逐字节一致。50个 Global、3200次命令尝试终结，25次输入容量背压，Host/Device 排空。

该配置的初始化完成时刻为3.046486µs；推理模拟时间12.839256µs，包括7.468µs固定计算（含0.005µs尾段）和5.371256µs所选访存服务。这些数字仅用于复现功能行为，不能视为硬件测量。

非零偏移对照选择同一tensor的`[4096,12288)`字节区间，计算间隔和尾段均为0；三方数值、覆盖与完成审计同样通过，推理模拟时间为5.371256µs，没有为零计算间隔额外增加Tick。

当前仅覆盖固定只读权重子范围、单生产者同步回调；无 KV 写回、权重更新、多GPU、异步计算/访存重叠、HBM容量/缓存或真实NAND字节模型。完整网络虽在真实 GPU 上执行，但只有清单中选中的范围接受 HBF 约束。一次通过不证明整个权重空间或长文本推理均已验证。

多 tensor 清单、覆盖率、GPU 计算区间测量和后续实验交付要求见[GPU 实验适配交接](gpu_experiment_handoff.md)。其中明确区分当前可复现能力与待实现任务。
