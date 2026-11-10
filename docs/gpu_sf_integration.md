# S/F 真实 GPU 实验集成

## 来源与结构

本轮从 [RealGPUAcess_v0 的 9d5b612](https://github.com/ZeePgm/RealGPUAcess_v0/commit/9d5b612)
导入已补齐的 S/F 源码，基于主仓真实 GPU 集成提交 `835d499` 整理。
后端继续位于仓库根目录；增量前端位于 `integrations/real_gpu`。
没有导入另一份 HBF 后端，也不应用后端 overlay。
llama.cpp、Detours 等已有前端依赖保留其许可证和现有修改。

- `experiments/gpu_s`：原有 S 接入及新增的批次路径配置。
- `experiments/gpu_ffn`：F1/F2/F3 权重范围选择、执行器、在线审计和复现入口。
- `experiments/cmake/cuda_experiment.cmake`：S/F 共用的 SDK、依赖库和当前后端构建设置。
- `experiments/reproduction_20261105`：批次脚本和固定上游来源哈希。

上述路径均相对 `integrations/real_gpu`。
[上游历史报告](../integrations/real_gpu/docs/gpu-s-f-new-backend/README.md)按原样保存；
其中实验数值和环境属于组员的原运行。本轮新验证单独记录。

## 接入契约

F 系列复用根目录的 `TensorAccessBridge → SimulationSession → Host → Device`。
HBF 参数通过 `read_configuration()` / `resolve_configuration()` 从 INI 读取，
输出完整 `effective.ini`。实际权重字节仍在 GPU，后端维护符号内容与资源状态。
同一矩阵的子命令可在后端并行，矩阵级父访问成功完成后才发出消费者计算。
冻结的 GPU 活动时间表与在线后端访问时延共同构造 token 模拟时刻。

保留此前本地集成的 CUPTI Kernel9/Kernel10 适配、完整时间表消费校验和 EOS 处理。
F 执行前后检查构建身份，包括当前后端、共享 CUDA 源码、SDK 和导入库；
F2/F3 复用 F1 原生记录时还检查同一实验构建身份。

## 构建与小实验

先按[原集成指南](gpu_integration.md)构建当前 checkout 的 integrated llama 依赖。
GPU 型号、CUDA 架构、CUPTI 和模型路径由运行机器决定。随后从仓库根目录执行：

```powershell
python integrations/real_gpu/tools/integrated_build_identity.py seal --build-dir build-sf-model
./integrations/real_gpu/experiments/gpu_ffn/build.ps1 -CudaRoot <CUDA目录> -BaseBuild ./build-sf-model -BuildDir ./build-sf-ffn -Python <Python路径>
python integrations/real_gpu/experiments/gpu_ffn/run.py --model <Qwen3-0.6B-F16.gguf> --build-dir build-sf-ffn --output build-sf-smoke --stage F1 --tokens 1
python integrations/real_gpu/experiments/gpu_ffn/validate.py --output build-sf-smoke --negative-checks
```

Python 需要 NumPy、PyYAML、tqdm（GGUF 读取依赖）；完整 S 数值参照另需其原环境依赖。
每次使用新的输出目录。正式 F 对照改用 8 tokens，并使 F2/F3 的 `--reference`
指向同一已通过的 F1 输出。[F 使用说明](../integrations/real_gpu/experiments/gpu_ffn/README.md)
包含完整矩阵。没有将模型权重、旧构建目录或大体量原始日志提交进源码。

## 验证

本轮验证结果见 [验证记录](validation/gpu-sf-integration-20261110.json)。
完整 F1/F2/F3 的八 token 长实验需要另外复跑；上游历史结果不能替代该项验证。

本轮已在 Windows / RTX 4070 Laptop / CUDA 12.9 上完成：

| 验证 | 结果 |
|---|---|
| S Python 回归 | 38/38 |
| F Python 合同（含构建身份负例） | 15/15 |
| CPU-only / CUDA 构建的 F CTest | 各 3/3；包含上述 F Python 套件 |
| F1 单 token 真实 GPU | 18 MiB 初始化 + 18 MiB 读取，3 次父访问 |
| 完整 logits / token | 与本轮原生 CUDA 逐位一致 |
| 片段与排空 | 294,912 写片段、294,912 读片段；零失败、零命令错误，Host/Device 排空 |
| 时间守恒 / 篡改负例 | 残差 0 ps；5/5 拒绝 |

该小实验未重新执行独立 CPU 数值参照，单 token 不定义 TPOT。
本轮原生及联合程序墙钟分别约 7.58 s、77.17 s，计入软件开销记录，
不计入模拟时间。原生冻结活动基线 TTFT 为 15.963683 ms，联合为
16.455632906 ms，增加 0.491949906 ms 的 HBF 同步访问时延。
