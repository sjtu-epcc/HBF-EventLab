# HBF-EventLab

**用于研究 Host 策略、设备资源与调度如何影响请求延迟和吞吐的 HBF 模拟器。**

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![arXiv:2611.19861](https://img.shields.io/badge/arXiv-2611.19861-b31b1b.svg)](https://arxiv.org/abs/2611.19861)
[![CI](https://github.com/sjtu-epcc/HBF-EventLab/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sjtu-epcc/HBF-EventLab/actions/workflows/ci.yml)

[English](README.md) | **简体中文**

HBF-EventLab 是基于 OCP HBF 规范的事务级离散事件模拟器。你可以运行读写 workload，在有限资源下比较不同策略，也可以通过增量 C++ API 接入外部执行模型。请求接收、写组装、介质执行、背压和完成反馈都在同一个 Host–Device 事件闭环中推进。

[论文](https://arxiv.org/abs/2611.19861) · [快速开始](#快速开始) · [实验](#实验) · [文档](#文档)

## 可以研究什么？

- **资源瓶颈：** 改变 outstanding 命令额度、Pending 写上下文数量和 Bank 资源，观察占用、组装超时与完成延迟。
- **地址与 Host 策略：** 比较 Global 地址交织、片段到达形态，以及 FCFS 与优先补齐已打开 DLU 的发送策略。
- **读写协同：** 改变读窗口释放和 NAND 调度策略，研究读延迟与写入进展。
- **故障恢复：** 注入 program 故障，观察 Host 驱动的重放、重试与逻辑请求完成。
- **应用层影响：** 从外部执行模型增量提交请求。论文中的真实 GPU 接入将测得的 GPU 活动时长与模拟 HBF 访问延迟组合，用于估算 TTFT 和 TPOT。

## 模型概览

| 组件 | 建模内容 |
|---|---|
| Host | Global 地址拆分为 64B 命令、发送策略、有界增量输入、重试与重放 |
| 写路径 | 4KiB 数据布局单元（DLU）组装、片段跟踪、顺序约束、超时、擦除与编程 |
| 读路径 | 64B 读取、Pending 转发、Regular/Batch 读窗口、顺序组，以及每 Bank 两个页缓存槽 |
| 资源 | 命令额度、Pending 上下文、Bank 占用、UCIe 传输和有限接收／返回缓冲 |
| 观测 | 逻辑请求与命令尝试延迟、Pending 生命周期、Bank 活动、JSON/JSONL 统计和可选墙钟性能诊断 |

模型以 [OCP HBF Architecture Specification v0.7.0](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf) 为依据，实现其中选定的行为子集。[设计决策与规范追溯表](docs/design_decisions.md) 将规范要求、模型选择、代码与测试对应起来。策略接口将调度选择与顺序、资源约束检查分开。

## 快速开始

后端需要 **C++17 和 CMake 3.20+**；完整测试还需要 **Python 3.10+**。核心库没有第三方运行时依赖。只有可选 GPU 实验需要 CUDA 和模型权重。

```sh
git clone https://github.com/sjtu-epcc/HBF-EventLab.git
cd HBF-EventLab
```

### Linux

安装支持 C++17 的编译器、CMake、Ninja 和 Python，然后运行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/hbf_eventlab_cli examples/workloads/normal.txt
```

### Windows

安装 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK、CMake 和 Python。在 PowerShell 中运行：

```powershell
.\build.ps1 -Configuration Release
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt
```

正常 workload 应输出 `STOP Completed workload=Success`，退出码为 `0`。Windows 脚本同时完成构建、测试和示例运行。

若只进行 C++ 测试，可在 CMake 配置时指定 `-DHBF_EVENTLAB_EXPERIMENT_TESTS=OFF`。其他构建方式和 CLI 选项见[使用说明](docs/usage.md)，已验证环境见[验证文档](docs/verification.md)。

### 配置与查看结果

Linux 示例：

```sh
./build/hbf_eventlab_cli examples/workloads/normal.txt \
  --config configs/functional.ini \
  --dump-config build/effective.ini \
  --stats-json build/normal-statistics.json

./build/hbf_eventlab_cli examples/workloads/recovery.txt --open 4 --fail-sequence 2
```

Windows 使用 `.\build\Release\hbf_eventlab_cli.exe`，将相同参数写在一行。

第一条命令保存实际生效配置与统计结果；第二条注入 program 故障并触发自动恢复。参数覆盖顺序为默认值 → INI → CLI。单位、合法性校验及可选策略见[参数文档](docs/parameters.md)。

## 实验

| 研究方向 | 问题与入口 |
|---|---|
| 资源限制 | 命令额度、Pending 容量与介质服务如何相互影响？[容量实验](experiments/capacity/README.md) |
| 交织与到达 | Global 交织如何改变 local DLU 的组装过程？[定时到达实验](experiments/interleave_arrival/README.md) |
| Host 发送 | 优先补齐已打开 DLU 在什么情况下与 FCFS 不同？[策略对比](experiments/arrival_policy/README.md) |
| 读写调度 | 读窗口与 NAND 仲裁如何影响竞争？[读调度实验](experiments/read_scheduling/README.md) |
| 真实 GPU 与 token 时延 | 选定 FFN 权重访问如何影响模拟 TTFT／TPOT？[GPU 分支的 F1–F3 实验](https://github.com/sjtu-epcc/HBF-EventLab/tree/feat/real-gpu-sf-experiments/integrations/real_gpu/experiments/gpu_ffn) |

后端实验脚本使用 Python 标准库；绘图依赖可通过 `python -m pip install -e ".[plots]"` 安装。各实验说明提供输入、配置、检查与输出解释，公共流程见[实验工具说明](experiments/README.md)。

**论文 GPU 实验：** F1–F3 实现在 `feat/real-gpu-sf-experiments` 分支中，与 `main` 上的后端分开维护。实验在真实 GPU 上运行 Qwen3-0.6B，真实权重字节由 GPU 执行器保留，HBF-EventLab 对选定权重访问进行建模。Token 时刻由冻结的 GPU 活动时间表与后端在线产生的 HBF 访问延迟组合得到。环境要求与复现入口见 [GPU 实验说明](https://github.com/sjtu-epcc/HBF-EventLab/blob/feat/real-gpu-sf-experiments/integrations/real_gpu/README.md)。

## 嵌入后端

有限 workload 文件可以通过 CLI 运行；C++ 程序可链接 **`HBFEventLab::core`** 增量提交 Global 请求：

- [在线会话](examples/online_session.cpp)：增量提交、模拟时间边界、完成反馈与输入关闭。
- [有界输入](examples/bounded_input.cpp)：驻留输入预算与容量通知。
- [Tensor 桥接](docs/tensor_bridge.md)：注册 tensor 范围，通过原有 Host 接入访存回调；链接 `HBFEventLab::tensor_bridge`。

公共头文件位于 [include/hbf_eventlab](include/hbf_eventlab/)。输入与模拟时间的协调规则见[在线 API 契约](docs/usage.md#online-session)。

## 适用范围

HBF-EventLab 是可配置的功能与时序模型，使用符号化 `DataToken` 表示内容。NAND 服务时间与缓冲深度是模型参数，Core Die Channel 当前采用理想带宽和时延。结果反映所选配置，尚未针对真实 HBF 产品校准。

当前后端不包含读 burst、完整 AXI/CSR/ADMIN、ECC 和温度模型。已支持响应与输入限制见[范围和限制](docs/scope_and_limitations.md)。实验应分别报告模拟时间与模拟器程序的墙钟运行时间。

## 文档

详细技术文档目前主要使用中文。

| 文档 | 内容 |
|---|---|
| [使用说明](docs/usage.md) | 构建、workload、CLI、在线 API 与退出码 |
| [参数说明](docs/parameters.md) | 统一配置、默认值、单位与校验 |
| [架构](docs/architecture.md) | 组件、地址、事件与 Host/Device 边界 |
| [设计决策与 OCP 规范追溯](docs/design_decisions.md) | 规范要求、实现选择与验证依据 |
| [验证](docs/verification.md) | 测试覆盖与复现命令 |
| [统计事件流](docs/statistics_event_stream.md) | 面向长时间运行的缓冲 JSONL 导出 |
| [程序性能诊断](docs/runtime_profiling.md) | 可选的模拟器墙钟耗时统计 |

## 引用

如果 HBF-EventLab 对你的研究有所帮助，欢迎引用我们的[论文](https://arxiv.org/abs/2611.19861)：

```bibtex
@misc{mei2026hbfeventlab,
  title         = {HBF-EventLab: A Discrete-Event Simulator for Studying Host-Device Coordination in High Bandwidth Flash},
  author        = {Ziqing Mei and Yiwei Huang and Zhiyi Zeng and Yuhao Xiao and Pu Pang and Quan Chen and Deze Zeng and Minyi Guo},
  year          = {2026},
  eprint        = {2611.19861},
  archivePrefix = {arXiv},
  primaryClass  = {cs.AR},
  url           = {https://arxiv.org/abs/2611.19861}
}
```

## 贡献与许可证

欢迎提交问题报告、策略实验、测试，以及规范解释的审阅意见。参与方式见 [CONTRIBUTING.md](CONTRIBUTING.md)。报告实验时，请附上代码版本、workload、实际生效配置与相关日志。

HBF-EventLab 采用 [Apache License 2.0](LICENSE)。
