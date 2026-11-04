# 读累积与 NAND 调度：最小实验

本实验实现读写调度竞争与读窗口释放的受控验证。固定 Host FCFS、1 channel / 1 die / 1 Bank、64B 命令，不启用 burst、故障或重试。结果用于解释当前功能模型的行为，不代表经过校准的硬件性能，也不据此宣称读优先普遍更好。

## 输入与固定条件

所有输入均为 Global 字节地址。首先通过正常写路径将 block 0 的四个 4KiB 页写好；程序检查初始化已在测量开始前完成。测量开始为 10 μs，统计 JSON 仍保留初始化历史，实验的逐请求延迟和 Bank 时间线仅提取测量阶段。没有直接植入 NAND 状态。

- sense 0.1 μs、program 0.5 μs、erase 1 μs、Cache access 100 ps，均为未校准功能参数。
- Selected/Supported MOCS 256、Pending 8、Host 打开上限 4；本轮不制造容量超载。
- UCIe、RX/TX 等采用统一配置默认值；每次运行保存完整 `effective.ini`。
- 写目标位于另一个独立逻辑 block。除 Cache 对照外，读不同已写入页，避免 Pending 转发、读写内容冲突和重复页命中干扰调度。
- `host.read_hint_mode=all_regular`；仅 Batch 对照显式使用 `tail_batch`。不能把每条 64B Global 读的默认尾部 Batch 当作累积窗口实验。

## 14 个运行案例

| 组别 | 输入与变化 | 检查内容 |
|---|---|---|
| 调度竞争，6 次 | W 在 10 μs 开始写入并触发 erase，R 在 10.2 μs 释放。NAND 策略 `fcfs/read_priority` × 窗口等待 0/50/1000 ns，数量触发关闭。 | 短窗口时 W 和 R 都在 erase 完成前就绪；读优先选择 sense，FCFS 选择较早的 program。长窗口使 R 错过调度机会。 |
| 不抢占对照，2 次 | 相同 W，R 改为 11.2 μs 释放，窗口等待为 0；两种 NAND 策略。 | program 已经发射，读优先不能抢占；两种策略的时刻一致。 |
| timer 与数量阈值，2 次 | 四个冷读在 10 μs 起每隔 40 ns 释放，窗口 200 ns；阈值分别为 0、4。 | 同一份 trace 中，阈值使第 4 个成员到达时释放；timer 从第 1 个成员进入时计算。 |
| 尾部不足阈值，2 次 | 三个读或仅一个读；窗口 200 ns、阈值 4。 | 没有后续输入也能由主动安排的 timer 释放。 |
| Batch 对照，1 次 | 一个 64B Global 读，显式 `tail_batch`。 | Batch 立即释放，不等待 timer。 |
| Cache 移除对照，1 次 | 同页的三个不同片段，间隔 40 ns，窗口 200 ns、阈值 2。 | 前两个读释放后只产生一次 sense；第三个读尚在新窗口时变为 Cache hit，移出窗口，取消无用 timer。 |

两种策略共用同一份 trace 文件；同一输入形态的窗口参数比较也共用文件。工具核对输入哈希及有效配置，允许的差异仅为 NAND 策略、读窗口参数、提示模式和输出位置。Host 策略不变。

NAND `read_priority` 只在 Device 已过滤的合法候选中优先选读，然后按现有 sequence/identity 排序。它不抢占、不会提前释放读窗口、不绕过同 Bank sense 顺序；program 与 erase 之间仍按 FCFS。持续读负载可能导致写饥饿，此策略未加入公平性上限。

## 复现

构建方式见[项目使用说明](../../docs/usage.md)。生成、运行和分析仅需 Python 3.10+ 标准库：

```powershell
python experiments/read_scheduling/run.py --cli build/Release/hbf_eventlab_cli.exe --output out/read-scheduling-minimal
```

Linux 使用对应的可执行文件路径，例如 `build/hbf_eventlab_cli`。输出目录必须不存在，工具拒绝覆盖已有结果。

绘图另需 matplotlib；安装到自己的 Python 环境后执行：

```powershell
python -m pip install -e ".[plots]"
python experiments/read_scheduling/plot.py out/read-scheduling-minimal
```

主要输出：

- `manifest.json`：案例、代码状态、CLI/脚本哈希、工具环境和完整性标记。
- `workloads/*.txt`：不同比较组共享的输入。
- 各案例的 `input.ini`、`effective.ini`、`statistics.json`、`console.log`、`result.json`。
- `reads.csv`：真实接受、窗口进入/离开、实际服务、响应交付及两种延迟。
- `window_releases.csv`：批次成员、触发原因、首成员时间、deadline、实际释放时刻。
- `bank_operations.csv`：测量阶段实际 erase/program/sense 操作。
- `summary.csv/json`：逐案例指标；`process_wall_seconds` 与模拟 Tick 分开。
- `figures/contention`、`window_gating`、`window_triggers`、`cache_removal`，各有 PNG/SVG。

## 预期结果与解释

当前功能参数下，50 ns 窗口的竞争例子中，R 的端到端延迟为 FCFS 1.403534 μs、读优先 0.903534 μs；W 的 program 完成时刻分别为 11.501767 μs、11.601767 μs。窗口 0 ns 与 50 ns 的实际介质顺序相同，因为两者都在 Bank 空闲前释放。窗口 1000 ns 时，两种策略都先 program，再 sense。

四个读的数量阈值释放发生于 10.120434 μs，timer 释放发生于 10.200434 μs。这里多出的 434 ps 是请求链路及到达开销：窗口从 Device 接受并进入集合时开始，不能用 workload `at` 代替。

单读和三读尾部均在第一个成员进入后的 200 ns 释放；不会因没有下一条输入而滞留。Cache 对照只有一次 sense、两次 Cache 服务；第三个读的 `window_left_at` 有值而 `release_reason=null`，表示被移出，不能冒充窗口释放。

每个案例都检查逻辑成功、完整排空、64B 尝试覆盖、时间次序、单 Bank 不重叠、真实候选竞争、窗口成员和实际服务之间的一致性。发现模型错误、输入不一致或损坏统计时，工具失败并保留诊断，不生成“成功实验”的图。

## 统计口径与边界

- 端到端延迟：该逻辑读最终完成减去原始释放；接受后响应延迟：交付减去 Device 接受。
- 窗口离开不等于 sense 发射。Cache 命中也可能在窗口释放之后出现。
- `read_paths[].service_at` 为实际 Cache pin 或成功 sense 发射；不是数据完成或响应交付。
- `read_window_releases` 由实际窗口组件发出；分析脚本不重新实现组装或窗口规则。
- 每次仅有少量读，报告逐请求、均值与最大值，不用本例估计有代表性的 p99。
- 本轮未实现容量压力下的读写协同大矩阵、写饥饿保护、读请求任意重排或真实 trace 适配。

## 自动验证

```powershell
python tests/test_read_scheduling_experiment.py
python tests/test_read_scheduling_experiment.py --cli build/Release/hbf_eventlab_cli.exe --output out/read-scheduling-tests
ctest --test-dir build -C Release -R "read_scheduling" --output-on-failure
```

Python 集成验证将完整 14 例重复两遍，核对逐事件统计确定性；另检查覆盖保护、输入公平性和损坏记录拒绝。C++ 测试覆盖候选排序、失效 timer、新窗口身份、末尾单读、Cache 移除、立即错误、暂停/继续、观察开关及非法统计事件。

公共工具、实际配置核对、可选绘图依赖和测试开关见[实验基础设施](../README.md)。
