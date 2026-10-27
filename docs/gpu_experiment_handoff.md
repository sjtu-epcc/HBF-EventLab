# GPU 实验适配交接：基于现有增量 Host

本文面向继续开展 GPU 实验的组员。起点为 `feat/incremental-host` 分支，目标是为论文提供可复现的真实 GPU 接入实验：扩大权重覆盖范围，核验数据与完成语义，并明确分解模拟时间和程序运行时间。

本任务延续当前同步访存适配方式，暂不实现有限显存驻留、真实 HBF 数据搬运、预取、多流重叠或完整 GPU 模拟。后端效率优化由另一条工作线负责。

## 1. 当前起点与实验结论边界

| 项目 | 当前实现与已验证范围 |
|---|---|
| 输入路径 | 现有 `HostDriver` 已支持增量输入、原子准入、活动逻辑片段容量、容量通知及 Global 最终完成反馈。不是旧 `StreamingHost`。 |
| 在线会话 | `SimulationSession` 管理原 Host、Device 和事件引擎，提供安全暂停、同 Tick 反馈交接、关闭输入与排空。 |
| Tensor 桥接 | `TensorAccessBridge` 支持注册多个不可变区域；一次父访存拆成多个 Global，经原 Host 执行。当前同步处理一次父访存。 |
| Qwen 实验入口 | `driver.cpp` 和 `SelectedRange` 目前只选择一个 tensor 的一个连续子范围；多 tensor 清单尚未实现。 |
| 实际数据 | 执行器预先将真实 FP32 权重加载到 GPU。HBF 初始化和读取符号内容，以 DataToken 和内容身份核验行为；不传输真实 NAND 字节。 |
| 默认验证 | Qwen3-0.6B 第一层 `q_proj` 的 8KiB；23 个 prompt token、1 个生成 token、24 个执行位置。包括独立 CPU、原生 GPU、HBF 接入 GPU 三方核验。 |
| 模拟计算时间 | 回调之间固定 1000ps、最后计算尾段 5000ps；尚不是测得的 GPU 计算区间。 |
| 程序诊断 | 已有默认关闭的 `RuntimeProfile`，测量拆分、登记、Host 推进、历史扫描、回收等耗时。尚未实施相应效率优化。 |

2026-10-26 的默认 GPU 验证中，两个 GPU 模式完整快照逐字节相同；CPU/GPU logits 最大绝对误差约 `6.53e-5`、RMSE 约 `7.25e-6`。初始化及读取共 50 个 Global、3200 次命令尝试终结，Host/Device 排空。非零选区偏移、零计算间隔及错误中止也有验证。复现条件见 [Qwen 实验说明](qwen_online.md)。

2026-10-27 的 Windows MSVC Debug/Release 核心及 Python 实验测试均为 454/454；最后的诊断补充检查为两种配置各 12/12。可选 CUDA 诊断入口已构建，但增加诊断后没有重新执行真实 GPU 三方实验。以上是已有验证记录，接收方应在自己的环境重新验证，不能由此推定其他平台已经通过。

本次交接提交前再次完成 Release 构建与全量回归：454/454 通过，CTest 墙钟 60.17 秒；这不是 GPU 实验的运行时间。

即使后续选择全部权重，也只能声称这些选定访问接受了当前 HBF 模型约束；不能声称真实权重驻留 NAND、节省了 GPU 显存，或已经测得真实 HBF 硬件性能。执行器回调是声明的 tensor 访存边界，不是逐条 GPU 指令或硬件缓存事务的完整追踪。

## 2. 必须沿用的主路径

```text
真实 Qwen CUDA 执行器
  ├─ 持有真实权重、执行数值计算、产生 tensor 访存回调
  └─ 实验适配器：清单匹配、范围相交、计算区间
       → TensorAccessBridge
       → SimulationSession
       → 现有 HostDriver：拆分、发送、重试/replay、保留与回收
       → 现有 DeviceDriver：MOCS、Pending、介质与链路资源
       → Global 最终完成 → 父访存完成 → 执行器继续
```

外部 [Small_case](https://github.com/ZeePgm/Small_case/tree/3d8a58ba1c321f41edc3280acda70388964a39c9) 目前仅提供 `src/qwen.cu`、`include/hbf_gpu/qwen.hpp`，以及权重准备和 CPU 参照脚本。构建不包含其旧 Host、旧在线运行器或旧后端。外部执行器源码没有复制到本仓库，构建时通过 `HBF_EVENTLAB_QWEN_SOURCE_DIR` 指定。

后续不得为了跑通大输入，另建并行 Host、直接向 Device 注入请求、读取 Device 私有状态补齐数据，或改成每页一个内容版本。也不采用旧实验的 `child_window=4`。当前 `--resident` 是 Host 活动逻辑片段预算；`--chunk` 是父访存拆成 Global 的字节数，二者都不是 GPU 缓存容量。

必须保留的契约：

- 初始化通过普通 Global 写路径，成功后才能读取；不能直接把 NAND 状态标记为已写。
- 物理地址和 NAND 细节留在后端；适配器使用 Global 字节地址，交织由原 Host 完成。
- 父访存等待所有已登记子 Global 的最终逻辑结果；命令尝试失败不必然等于 Global 最终失败，恢复仍归原 Host。
- 输入背压表示尚未登记；保留待提交范围，容量通知后重新检查，不重复提交已经登记的工作。
- 外部输入不得回填到过去或已结算的 Tick；零计算间隔使用已有同 Tick 因果交接，不手工加一个 Tick。
- `LimitReached`、`WaitingForInput`、契约错误不能算成功。末尾显式推进计算尾段、关闭来源并检查排空；排空也不能替代逐父访存成功检查。

完整 API 语义见 [Tensor 桥接契约](tensor_bridge.md) 和 [在线输入用法](usage.md)。

## 3. 从哪些文件开始

| 文件／入口 | 当前职责 | 本任务的主要关注点 |
|---|---|---|
| [access_plan.hpp](../experiments/qwen_online/access_plan.hpp)：`SelectedRange::validate/intersection` | 单选区校验与回调相交 | 扩展清单、别名处理、多选区相交与覆盖验证。 |
| [driver.cpp](../experiments/qwen_online/driver.cpp)：`arguments`、`inventory`、`main` 中的访存回调 | 参数、默认几何、注册与初始化、GPU 回调、时间推进、输出 | 接入多 tensor 清单、计算时间输入、多个子访问归属与统计。 |
| [run.py](../experiments/qwen_online/run.py) | CPU/native/HBF 三组运行、哈希与验证报告 | 扫描矩阵、固定输入和计算表、记录配置与退出结果。当前未透传全部直接运行选项。 |
| [verify.py](../experiments/qwen_online/verify.py)：`numerical`、`timeline`、`behavior` | 数值、时间、地址与完成审计 | 消除单 tensor／单次初始化假设；保持拒绝错误结果的检查强度。 |
| [measure_wall.py](../experiments/qwen_online/measure_wall.py) | 历史墙钟复现 | 该脚本还要求旧实验可执行文件用于对照，不作为新 GPU 实验矩阵的主入口。 |
| [tensor_access_bridge.hpp](../include/hbf_eventlab/adapters/tensor_access_bridge.hpp)、[实现](../src/adapters/tensor_access_bridge.cpp) | `initialize/access/advance_compute/finish`，父访问聚合 | 优先复用，必要接口扩展与后端负责人协商。 |
| [simulation_session.hpp](../include/hbf_eventlab/runtime/simulation_session.hpp) | 安全推进与输入/完成反馈 | 沿用，避免实验脚本另行驱动内部事件或修改 deadline 规则。 |
| [test_qwen_online.py](../tests/test_qwen_online.py)、[compute_tail.cpp](../tests/compute_tail.cpp)、[tensor_bridge.cpp](../tests/tensor_bridge.cpp) | 无 GPU 的审计、计算尾段和桥接验证 | 增加清单、别名、覆盖、时间表、失败父访问等测试。 |
| [runtime_profiling.md](runtime_profiling.md) | 后端程序耗时诊断 | 按需开启；不能把嵌套计时直接相加，也不能将程序耗时当模拟时延。 |

实际 CUDA 区间测量需要执行器侧的 kernel/stream 边界；仅靠当前 `driver.cpp` 的 CPU 回调计时不够。外部执行器如需修改，应保存可复现的补丁和基准提交，记录补丁哈希，避免只有某台机器的工作副本包含改动。若要复制或分发外部源码，先核对其授权范围。

## 4. 工作包 A：多 tensor 清单和覆盖率

建议保留原单选区参数用于复现，在实验层增加一个版本化清单入口。以下是待实现的清单内容，不是当前已支持的 CLI：

- 模型修订、权重/布局哈希、清单版本。
- 每个存储区域的唯一 ID、tensor 名称、源偏移、有效字节数、Global 基址、注册长度、content。
- 真实存储别名关系、用于对齐的 padding，以及初始化顺序。

当前 `TensorAccessBridge` 已接受多个 `TensorRegion`，但拒绝重叠注册；同一逻辑 block 中的区域必须使用相同 content。适配器应将共享权重归到一个 canonical 区域，再让别名访问引用它。例如当前执行器使用共享 embedding 处理输出投影，不能把 `lm_head.weight` 再算成一份独立存储。

初版优先采用一 channel、合法连续布局，并逐步扩大到完整 tensor／代表性层。布局必须覆盖完整 local DLU、满足同 block 前页接收条件；仅有 64B 对齐并不保证初始化能前进。尾部 padding 必须显式登记和初始化，并从有效权重覆盖率中扣除。提供外部 INI 时应校验容量，不静默扩容。多 channel 时另验交织后的覆盖和 Host 预算，不能把 Global 4KiB 自动当成一个 local DLU。

一次回调如果命中多个已注册范围，必须保存“回调 → 范围 → 桥接父访问 → Global”的完整归属。初版可以确定性地串行调用现有桥，但必须报告这种串行语义；计算间隔只在原回调边界计一次，不能给每个交集重复计计算，也不能据此宣称子范围并行执行。

至少分别输出：

| 指标 | 口径 |
|---|---|
| 注册存储覆盖率 | 已注册的唯一有效权重字节／去除真实别名后的完整权重字节；不计 padding。 |
| 选区访问覆盖 | 本次运行访问区间的并集，与声明选区逐项比较；未触达区域显式列出。 |
| 回调服务覆盖 | 按清单本应交给 HBF 的回调交集，是否全部被服务；不能遗漏后只汇报总体比例。 |
| 累计访问流量 | 回调重复访问产生的有效字节，重复计入；与唯一存储字节分开。 |
| 后端实际工作量 | 初始化写、推理读、重试/replay、命令数、Global 数、介质操作数分别记录。 |

硬编码源常量应尽量改从实际计划、模型布局、有效配置或已有公共接口读取。审计逻辑需要独立计算期望相交和覆盖，不能只是照抄执行器已经导出的计数。

## 5. 工作包 B：真实 GPU 计算区间与时间分解

建议先在原生 GPU 路径上预热并测量，再冻结一份计算时间表，供相同输入的 HBF 对照共用。时间表按实际回调序列/执行位置索引，不只按 tensor 名称：同一 tensor 在不同位置可能重复执行。记录模型、prompt、精度、回调顺序及哈希；序列不匹配应拒绝，不能静默套用错误区间。

采用 CUDA event 或等价的 GPU 时间戳测量已界定的 stream 内计算区间。边界从上次访存门控返回后开始，到下一次访存门控前结束；另测最后一次访存后的计算尾段。不要让开始/结束事件跨过 CPU 后端等待；文件快照、Host 模拟推进和 CPU 编排也不能混进所谓“纯 GPU 计算”。参考 [NVIDIA CUDA 计时说明](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#timing)。

当前固定时间模式继续保留，新增实测时间表模式须注明测量平台、预热/重复次数、使用中位数还是逐次样本，以及从测量单位转换到 ps 的规则。不能为了避免零值随意加 Tick，也不能把测不准的微小区间宣传为高精度结果。

至少分开报告这些量：

| 类别 | 指标与边界 |
|---|---|
| 模拟时间 | 初始化时刻/耗时；推理中显式 GPU 计算（含尾段）；选中访存的模拟等待；推理区间总 Tick。当前同步模式下计算与等待串行，不宣称重叠收益。 |
| 实际 GPU 时间 | 原生 GPU 计算区间测量值；与 CPU 进程墙钟分开。 |
| 程序墙钟 | 模型加载、HBF 初始化、推理、后端推进、验证/快照/文件 I/O；嵌套项注明包含关系。 |
| 后端诊断 | `RuntimeProfile` 的 inclusive/self 和工作量计数，单独作为实现效率证据。 |

当前 `executor_and_dump_wall_seconds` 包含同步、CPU 编排和快照输出，不能直接改名为 GPU compute。HBF 默认媒体与链路模型仍未经过真实 HBF 校准，即便 GPU 计算时间实测，也只支持“给定模型参数下”的结果。

## 6. 工作包 C：数值、行为、实验矩阵和图表

保持三方验证：独立 Transformers CPU 参照、原生 CUDA、接入 HBF 的 CUDA。固定模型修订、输入 token、精度、生成策略和输出长度；先声明误差阈值，再运行实验，不根据结果放宽。当前两个 GPU 路径采用相同计算顺序时要求完整快照逐字节一致；CPU 对照按现有阈值检查 hidden/KV/logits、token 与停止原因。

至少新增以下功能用例：多 tensor、一个回调覆盖多个选区、非零偏移、未选区不提交、别名去重、非法重叠/越界/内容冲突、容量不足后的继续提交、任一子访问失败的父失败、计算尾段和最终排空。先做不依赖 CUDA 的清单与时间表单测，再做小 GPU 数值验收。不能只比较最后一个生成 token。

建议按下列小矩阵交付，不以全权重详细模拟为首轮门槛：

| 组别 | 内容 | 用途 |
|---|---|---|
| G0 | 原 8KiB、非零偏移和零计算间隔基线 | 证明扩展没有破坏已验证路径。 |
| G1 | 同一选区逐步增大到完整 tensor；再选多个 tensor／代表性层 | 验证覆盖与规模变化；保持后端配置可比并明确容量调整。 |
| G2 | 同一选区下固定时间与测得计算表对照 | 验证时间分解和计算表接入，不把二者解释为真实 HBF 测量。 |
| G3（可选） | 相同有效权重字节预算的两种选区布局 | 探索位置差异；与“覆盖更多字节”的规模实验分开。当前同步 dense 路径下差异很小也应如实报告。 |

规模先按实测程序耗时逐档放大，设置明确墙钟预算。预算用尽的结果标记为未完成，并保存最后进度，不能当成失败模型已排空或成功完成。较大选区已经观察到明显后端运行成本；效率工作线改进后再扩大矩阵。

正确性验收使用完整事件审计；程序耗时测量再采用 `--detail off` 等明确记录的观测配置。关闭详细记录的样本不能声称经过完整事件审计。大输入可采用固定检查点的数值验证，但需保留小输入完整快照验证，写明大输入抽查范围。

图表优先包含：选区规模与覆盖率；模拟计算/访存等待时间分解；实际程序墙钟及后端推进分项；不同规模的峰值进程内存。程序墙钟重复测量并报告离散程度，不能以模拟 Tick 代替模拟器程序速度。可比较 native 与 HBF 接入程序开销，但不能把其比值直接解释为未来 HBF 硬件 slowdown。

每次运行保存：代码/外部执行器/补丁版本、编译配置、GPU/驱动/CUDA、模型和 prompt 哈希、清单、有效 INI、计算时间表、日志级别、结果、运行状态及图表生成参数。复用 `experiments/common` 的配置、IO 和来源记录能力，不再建立另一份公共工具实现。

## 7. 接收后的最小复现顺序

新工作副本可从交接分支建立自己的实验分支，避免与后端优化同时改同一目录：

```sh
git clone --branch feat/incremental-host https://github.com/sjtu-epcc/HBF-EventLab.git HBF-EventLab-gpu
cd HBF-EventLab-gpu
git switch -c exp/gpu-coverage-timing
git rev-parse HEAD
```

先验证 CPU 桥接和在线契约。以下为 Windows PowerShell 示例，需 C++ 工具链、CMake 和实验测试所需的 Python；GPU 不是这一步的依赖：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release -R '^(session_handoff_|tensor_bridge_|tensor_compute_|runtime_profile_|experiment_qwen_audit_unit$)' --output-on-failure
.\build\Release\hbf_eventlab_tensor_bridge_demo.exe
```

随后按 [Qwen 环境与构建](qwen_online.md#环境与构建) 准备外部执行器、固定权重和 Python 环境。外部执行器基准提交为 `3d8a58ba1c321f41edc3280acda70388964a39c9`；CUDA 架构参数应匹配接收方 GPU，不能照抄本机值。

完成构建后，原有最小三方验证命令如下；变量替换为接收方实际路径，输出目录必须为空或不存在：

```powershell
$python = '../qwen-env/Scripts/python.exe'
$executor = '../Small_case'
$model = '../models/Qwen3-0.6B'
& $python experiments/qwen_online/run.py `
  --program build-qwen/experiments/qwen_online/Release/hbf_eventlab_qwen.exe `
  --executor-source $executor --model $model `
  --output build-qwen/gpu-handoff-baseline --failure-checks
```

当前 `run.py` 仍是单选区三方入口。多 tensor 矩阵与计算时间表是本任务的交付内容，不是现成参数。直接运行 Qwen 程序可使用 `--profile on` 导出程序诊断；开关和范围见 [运行效率说明](runtime_profiling.md)。检查工具链/数据缺失应明确失败，不用 CPU 替代 GPU 后宣称验证成功。

权重、大型数值快照、构建目录和实验原始数据不纳入 Git；在被忽略的输出目录运行，交付脚本、清单、小配置与汇总图表，并为原始结果提供团队约定的可访问位置和哈希。

## 8. 分工与交付检查表

| 负责人 | 主要范围 | 交付 |
|---|---|---|
| GPU 实验组员 | `experiments/qwen_online`、对应测试、必要的外部 CUDA 测量补丁、实验文档 | 多 tensor 清单与审计；固定计算表；三方结果；可复现矩阵、时间分解和图表。 |
| 后端效率负责人 | Host 历史扫描、候选推进、回收、输入准备，以及程序诊断 | 保持模拟语义的优化；等价性测试；墙钟与内存规模曲线。 |
| 共同确认 | 公共桥接/API、统计字段与时间契约 | 任何公共接口变更先说明动机与兼容方案；不得为优化改变请求粒度、内容身份或资源预算。 |

接收方先复现 G0，再独立开展清单与测量；不必等待后端效率优化。后端合并后，同一实验配置应保持语义结果，程序墙钟可以改变。公共接口尽量保持稳定，两个分支通过一个固定小验收案例持续对接。

- [ ] CPU 桥接和 G0 GPU 基线复现，失败样本有明确诊断。
- [ ] 清单区分有效字节、padding、别名和实际覆盖，初始化满足 local DLU 与内容身份契约。
- [ ] 至少一组多 tensor 运行通过三方数值与公开事件行为审计。
- [ ] GPU 计算时间表包含尾段，排除后端等待与快照开销，能检查输入匹配。
- [ ] 图表明确区分模拟时间、实测 GPU 时间和程序墙钟，并给出参数与覆盖范围。
- [ ] 提供一条可复现矩阵的运行命令、结果清单、源码/配置/输入哈希及已知限制。

容量模式和复杂异步数据路径另行立项，不作为上述交付的隐含要求。
