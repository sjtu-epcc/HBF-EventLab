# Global 交织 × 片段释放间隔

固定地址递增输入、FCFS 和充足的命令／Pending 预算，改变交织粒度与 Global 64B 条目的释放间隔，观察每个 local DLU 的输入可用时间、实际组装、超时与 Bank 占用。实验复用现有 CLI、配置入口和统计观察，不增加另一套组装状态机。

这些是合成 workload 和未校准的功能时延，用于验证机制与定位瓶颈，不代表真实应用流量或 HBF 硬件性能。

## 固定条件与矩阵

| 项目 | 设置 |
|---|---|
| 拓扑 | 4 UCIe channel；每 channel 1 die、1 Bank；Core Channel 沿用理想资源模型 |
| 介质几何 | 每 Bank 4 个可见 block、1 个备用 block；每 block 8 页；每页 4KiB |
| Host | `fcfs`、`fragment64`，不启用 burst；每 channel 最多打开 16 个 DLU |
| 接收预算 | 每 channel Supported/Selected MOCS 均为 1024，Pending 上限 16 |
| 时序 | 组装期限 10μs；erase 1μs；program 0.5μs；Tick = 1ps |
| 错误处理 | 无故障注入，Pending/timeout 自动重试和 replay 预算均为 0 |
| 输入 | 从 Global byte 0 开始连续写；每条 64B；同一逻辑内容版本 |
| 输入大小 | 16KiB（每 channel 1 个完整 DLU）；128KiB（每 channel 1 个完整的 8 页 block） |
| `interleave_bytes` | 64、256、1024、4096B |
| Δ | 0、1、4、16、32、50、128、160ns |

[round1.json](plans/round1.json)包含 **2 × 4 × 8 = 64 组**；[smoke.json](plans/smoke.json)保留 16KiB、64/4096B 交织和 0/50ns 间隔，共 4 组。

所有矩阵格使用相同资源预算。其他配置保持 CLI 默认值，包括有限 RX/TX 和链路开销；每格均导出完整 `effective.ini`。MOCS、Pending 充足并不表示链路、Host 顺序约束或 Bank 没有等待。

计划字段由 [plan.py](plan.py)校验；支持 1/2/4/8 channel、64—4096B 的二次幂交织、完整的 `channel数 × 4KiB` 输入组，最多覆盖每 channel 一个 8 页 block。这个范围保证本组只考察连续完整地址集合；不在此工具中扩展到跨 block 补齐策略。

## 输入与公平性

第 `i` 条记录（从 0 开始）为：地址 `64*i`、稳定 ID `i+1`、内容标识 1、`at=i*Δ`。这里的 `at` 是 **Host 端输入释放时刻**，不是链路移交、RX 到达、Device 接受或 Assembly 收片时刻。

相同输入大小、相同 Δ 的全部交织配置共用同一个 trace 文件，逐字节一致；交织只通过设备配置改变。改变 Δ 会有意改变输入时间分布；改变交织会有意改变 Global→local 的对应关系。相同大小的配置最终覆盖相同的 local 地址集合，每个 local 页恰有 64 个不同的片段。分析脚本逐条核对模拟器输出与独立地址计算结果。

需要对比`fcfs`与`fill_open`时，可运行[第二组最小例子](../arrival_policy/README.md)：成批释放已打开DLU的补片与新DLU首片，另外提供没有候选竞争的稀疏对照。

Δ=0 时全部记录同 Tick 释放，文件次序是地址递增；Δ>0 时次序由释放时间确定。不会在重新排列记录之后偷偷重新分配 `at`。本组只使用一种 Host 策略，因此不能用结果宣称 FCFS 或 `fill_open` 更优。

## 可解析的对照量

令 channel 数为 `C`，交织大小为 `I`，`k=I/64`。在本实验的对齐、连续地址、`I≤4096` 且整除 4096B 的条件下，一个 local DLU 从首片到末片的**计划释放跨度**是：

```text
S_release = [64*C - (C-1)*k - 1] * Δ
```

4 channel 时，四种交织对应 `252Δ、243Δ、207Δ、63Δ`。例如 Δ=50ns：64B 交织的跨度为 12.6μs，4096B 交织为 3.15μs。工具既枚举各片段映射，也检查此公式，随后与真实 CLI 输出的地址和时间比较。

**这不是实际组装时长，也不是充分的超时判据。** 组装期限从 Device 建立上下文开始；Host、链路和接收等待会改变收片时间分布。尤其在 Δ=0 时，输入释放跨度为 0，实际组装时间仍大于 0。应结合 `opened_at`、`deadline`、`ready_at` 和实际片段轨迹解释结果。

## 运行

先按项目 [构建说明](../../docs/usage.md#构建)生成 CLI，再在仓库根目录运行。生成、扫描、分析只需 Python 3.10+ 标准库：

```powershell
python experiments/interleave_arrival/run.py --cli build/Release/hbf_eventlab_cli.exe --plan experiments/interleave_arrival/plans/smoke.json --output out/interleave-arrival-smoke
python experiments/interleave_arrival/run.py --cli build/Release/hbf_eventlab_cli.exe --output out/interleave-arrival-round1
```

Linux 单配置构建通常使用 `--cli build/hbf_eventlab_cli`。输出目录必须尚不存在，工具不会覆盖既有实验。`--timeout-seconds` 是每个 CLI 进程的墙钟上限，默认 120s；计划中的 `max_tick_ps`、`total_events` 是模拟运行限制。`--max-cases` 仅运行矩阵前缀，未覆盖全部矩阵时 manifest 不会标记完整扫描。

CLI exit 0/2/3/4 分别保存为成功排空、失败排空、停滞、达到限额。预期逻辑失败不算脚本故障；输入／模型错误、缺失或损坏统计、进程超时会记为 `harness_error`，不会伪造一个成功率为 0 的实验格。

## 输出与指标口径

根目录保存 `manifest.json`、`summary.csv/json` 和共享 `workloads/`。manifest 记录计划、Git 版本与脏工作区标记、CLI/脚本/输入 SHA-256、Python 和平台信息。脏工作区的 commit 本身不能完整标识源码，应保留对应源码版本。

每个 `cases/n<size>-i<interleave>-d<delta_ps>/` 保存输入配置、有效配置、原始 `statistics.json`、CLI 日志、命令／哈希／结果记录，以及下列 CSV：

| 文件 | 用途 |
|---|---|
| `release_schedule.csv` | 全部计划地址、释放时刻和独立计算的 local 映射，包含尚未释放即被取消的工作 |
| `dlus.csv` | 每个原始 local DLU 的最终结果、首末计划释放、首次 opened/ready/program、成功 program 与逻辑终结时刻 |
| `fragments.csv` | 每个原始片段的计划／实际释放、移交、RX、接受、交付、取消原因及结果 |
| `contexts.csv` | 实际打开的上下文，含 generation、期限、阶段时刻、收片数、终结原因 |
| `progress.csv` | 已有统计接口输出的逐上下文真实收片进度 |
| `pending.csv` | 逐 channel 的原始 Pending 状态变化，保留同 Tick 的 sequence |
| `bank.csv` | 实际 erase/program 的发射、释放与忙时长 |
| `channels.csv` | 各 channel 的首个 Ready、资源峰值／时间积分／利用率等摘要 |
| `resources.csv` | 同一时间轴上的 MOCS、Pending 三阶段、Host 已释放未移交数量和 Bank 忙闲 |
| `bank_parallelism.csv` | 全设备同时繁忙的 Bank 数，用于检查多 channel 并行 |

主要口径如下：

- **原始 DLU 成功率**：64 个逻辑片段全部成功的原始 DLU 数／计划 DLU 数。未发送、取消、失败和未完成均保留在分母中；限额停止另报 unfinished。
- **上下文超时比例**：以 `assembly_timeout` 终结的上下文数／实际打开上下文数。它不是 `1−DLU成功率`；一个逻辑 DLU 和一次上下文生命周期是不同统计对象。
- **Ready 时间**是 Device 真实收齐；成功 program 时间与 Host 逻辑工作最终完成时间另列，不混作“首个完成”。
- **MOCS 占用**来自接受至响应交付；**Host 队列**来自实际释放至移交，未移交就取消的记录止于取消。不把 AcceptedNotice 当作 Assembly 保留片段的证明。
- `resources.csv` 将同 Tick 变动合并为该 Tick 结束后的阶梯值，MOCS/Host 队列峰值因此标为 `*_tick_boundary`；零时长瞬态不计面积。原始 Pending 的细粒度事件仍在 `pending.csv`。
- **Bank 利用率**按 `[0, simulation_end]` 计算。每 channel 一个 Bank；设备平均利用率分母为 `C × simulation_end`。Bank 忙包含 erase 与 program，表中也分别记录两者，不能把跨 Bank 忙时长之和除以单 Bank 时间。
- 超时时记录该上下文是否仍有**计划于 deadline 之后释放**的片段。这是输入侧证据，不单凭这个标签断定策略因果；已计划释放的片段也可能仍在 Host、链路或 RX 等待。

关闭重试后，Host 可能在首个 DLU 超时后终止同块当前内容版本，取消后续页。因此 **32 个原始 DLU 失败可能只对应 4 次实际上下文超时**。计划释放表始终完整；取消事件可使模拟提前排空，其结束时刻不能被当作成功传完相同数据量的性能成绩。

## 绘图

绘图是独立后处理，需要可选绘图依赖 matplotlib 和 NumPy；不影响核心构建和 CTest：

```powershell
python -m pip install -e ".[plots]"
python experiments/interleave_arrival/plot.py out/interleave-arrival-round1 --case n16384-i64-d50000 --case n16384-i4096-d50000 --case n131072-i64-d0 --case n131072-i4096-d50000
```

生成 PNG/SVG：两种大小各一张四面板总览、解析释放跨度图，以及所选案例的逐 channel 资源曲线和“计划释放／实际组装”时间图。没有 Ready 时刻显示 N/A；未完成或工具错误的格不填充为有效实验结果。输出位于实验目录的 `plots/`。

## 已观测结果

默认 64 组均完成并通过输入／统计一致性检查：44 组所有原始 DLU 成功，20 组出现预期的组装超时与逻辑失败，没有工具错误。两种输入大小具有相同的这次离散扫描边界：

| 交织 | 全部成功的 Δ 样本（ns） | 出现超时的 Δ 样本（ns） |
|---|---|---|
| 64 / 256 / 1024B | 0、1、4、16、32 | 50、128、160 |
| 4096B | 0、1、4、16、32、50、128 | 160 |

这些是已取样的点，并非精确临界值。所有运行中每 channel 的 MOCS 峰值不超过 512、Pending 峰值不超过 8，未达到本组 1024/16 的上限，也没有 Pending-full 响应。

Δ=50ns 的对照：

| 输入 | 交织 | 成功原始 DLU | 实际超时上下文 | 首个 Ready |
|---|---:|---:|---:|---:|
| 16KiB | 64B | 0/4 | 4 | 无 |
| 16KiB | 4096B | 4/4 | 0 | 3.151767μs |
| 128KiB | 64B | 0/32 | 4 | 无 |
| 128KiB | 4096B | 32/32 | 0 | 3.151767μs |

由此可以观察到：在地址、内容、Host 策略和资源预算固定时，交织改变了 local DLU 的补片可用时间分布，并在当前期限和接收流程下改变了是否超时。它不说明粗粒度交织总是更好；还需结合多 Bank 并行度、首个 Ready 与成功 workload 完成时间解释其取舍。

## 验证

```powershell
python tests/test_interleave_arrival_experiment.py
python tests/test_interleave_arrival_experiment.py --cli build/Release/hbf_eventlab_cli.exe --output out/interleave-arrival-tests
ctest --test-dir build -C Release -R '^experiment_interleave_arrival_' --output-on-failure
```

11 项 Python 单元测试覆盖矩阵合法性、固定预算、地址覆盖、释放公式、共享 trace、区间积分与缺失字段。真实 CLI 集成检查覆盖正常／超时、完整块取消传播、单 channel 退化对照、同 deadline 补齐及 1ps 越界、限额中止、多 Bank 并行利用率、确定性、损坏统计拒绝和输出覆盖保护。完整项目回归记录见[验证说明](../../docs/verification.md)。

公共工具、实际配置核对、可选绘图依赖和测试开关见[实验基础设施](../README.md)。
