# 到达形态 × 补齐策略：最小对照

这是实验二第二组的最小例子：在**完全相同的地址、内容、`at`、文件顺序和资源预算**下，比较 `fcfs` 与 `fill_open`。一组输入制造可选择的候选，另一组稀疏输入用作无竞争对照，总计 **2 种到达形态 × 2 种策略 = 4 次运行**。

本例复用现有 Host、Device、组装状态与统计接口，不修改模拟器 C++ 行为。生成与验证仅依赖 Python 标准库；图形是可选后处理。

## 固定条件

- 1 UCIe channel、1 die、1 Bank，消除跨 channel 和跨 Bank 的变量；交织固定为 64B。
- 一个 block 有 8 个 4KiB page；使用两个**独立 block 的首页** A、B，Global byte 地址分别为 0、32768。实际数据量为 8KiB，地址范围中间有空隙。
- 每个 page 写 64 个不同的 64B 片段，`fragment64`，不启用 burst；同一内容版本。
- Host 打开 DLU 上限 2；MOCS Supported/Selected 为 256；Pending 上限 8。
- 组装期限 10μs；erase 1μs，program 0.5μs；无故障，关闭资源重试与 replay。
- NAND 调度固定 FCFS，链路调度固定 FIFO，其余参数由 `effective.ini` 完整保存。

这里固定一个 channel 是为了隔离 Host 选择。多 channel 交织本身的影响见[第一组实验](../interleave_arrival/README.md)。

## 输入如何制造选择

成批输入 `batched`：

| 文件位置 | Host 释放时刻 | 请求 |
|---|---:|---|
| 1—32 | 0μs | A 的片段 0—31 |
| 33—64 | 2μs | B 的片段 0—31 |
| 65—96 | 2μs | A 的片段 32—63 |
| 97—128 | 4μs | B 的片段 32—63 |

到 2μs 时，A 已实际打开并收到 32 个片段，B 尚未打开。B 的首片和 A 的补片同时可用，文件中 B 在前：

- `fcfs` 在合法候选中选队龄最早的记录，因此先发送 B 的前半页，再补 A。
- `fill_open` 优先选已打开 DLU 的补片，因此先补完 A，再发送 B。
- B 的前半页被发送后，后续 B 片段也属于补片；此例不以“只有 A 能被识别为补片”为假设。

稀疏输入 `sparse` **保持上述地址、内容、ID 和文件顺序不变**，仅设置第 `i` 条记录 `at=i×100ns`（从 0 计数）。当前链路可在下一条到达前移交上一条，Host 没有多个候选竞争，故两种策略应相同。

改变到达形态会有意改变输入时刻；**同一形态比较两种策略时不会修改任何 `at`**。两种策略共用同一个 trace 文件，脚本校验 SHA-256 一致；有效配置也逐项比较，只允许 Host 策略和输出路径不同。不得把本例的 Host 策略差异误称为改变了 NAND Scheduler。

## 运行与输出

在仓库根目录执行；可执行文件按实际构建目录调整：

```powershell
python experiments/arrival_policy/run.py --cli build/Release/hbf_eventlab_cli.exe --output out/arrival-policy-minimal
```

Linux 单配置构建通常使用 `--cli build/hbf_eventlab_cli`。输出目录必须尚不存在。此工具是预期全部成功的有限示例；任一运行失败、记录损坏或对照检查不成立，工具会报错并保留现场，而不会当作一个有效的性能结果。

- `workloads/batched.txt`、`sparse.txt`：共享 trace；相邻 CSV 明确标注页名、地址与释放时刻。
- `summary.csv/json`：移交次序、A/B 的 Ready、program 与逻辑完成时刻、整体完成时间、平均片段延迟及资源占用。
- `<shape>/<policy>/`：原始统计、输入／有效配置、CLI 日志、运行命令与哈希；另外导出 `handoffs.csv`、`pages.csv`、`progress.csv`、`resources.csv`、`bank.csv`。
- `manifest.json`：版本、CLI／工具哈希、运行环境与对照检查状态。Git 工作区有修改时，应同时保留对应源码，不能仅靠 commit 标识复现。

时刻以 ps 保存。移交次序来自真实 Host handoff；Ready 和收片进度来自真实 Assembly 观察。MOCS 从接受至交付计账，Pending 与 Bank 来自已有记录；不将接受通知解释为收片。

`A32 B32 A32 B32` 表示按页名合并相邻移交记录，不是四个 2KiB 命令；实际仍为 128 条 64B 命令。资源曲线合并同 Tick 变动后的值；原始统计保留更细的事件次序。

## 已观测结果

四次运行均成功写入 A、B，总计每次 128 条命令、2 次 erase、2 次 program。MOCS/Pending 峰值不超过 128/2，没有触及容量限制。

| 形态／策略 | 移交次序 | A Ready（μs） | A program 完成（μs） | 整体逻辑完成（μs） |
|---|---|---:|---:|---:|
| batched / fcfs | A32 → B32 → A32 → B32 | 2.106788 | 3.501767 | 4.574920 |
| batched / fill_open | A64 → B64 | 2.053444 | 2.553444 | 4.574920 |
| sparse / fcfs | A32 → B32 → A32 → B32 | 9.501767 | 10.001767 | 13.223243 |
| sparse / fill_open | A32 → B32 → A32 → B32 | 9.501767 | 10.001767 | 13.223243 |

成批输入中，`fill_open` 使 A 提前约 **53.344ns 收齐、0.948323μs program 完成**。差距不仅来自传输次序：FCFS 先让 B 的首页进入 Device，Bank 从 2.001767μs 开始 erase B，A 随后虽收齐却需等待；`fill_open` 先让 A 就绪并 program，然后再 erase B。两种策略使用的是同一个 NAND 调度器，只是候选的到达时间不同。

**整体完成时间相同。** B 的后半页都要到 4μs 才释放，两种策略最终都在 4.553444μs 完成 B 的 program，并在 4.574920μs 交付全部响应。因此本例证明策略有选择效果，并不证明补齐优先一定提高吞吐。成批输入的平均原始片段端到端延迟从 2.0385605μs 降到 1.564399μs，而 Bank 总忙时长均为 3μs。

这些固定时延是未校准的功能参数。本例适合确认因果路径；尚未扫描 batch 大小、间隔、资源压力、多 Bank 或公平性，不能据此判断通用最佳策略。

## 绘图与测试

```powershell
python -m pip install matplotlib
python experiments/arrival_policy/plot.py out/arrival-policy-minimal
python tests/test_arrival_policy_experiment.py
python tests/test_arrival_policy_experiment.py --cli build/Release/hbf_eventlab_cli.exe --output out/arrival-policy-tests
ctest --test-dir build -C Release -R '^experiment_arrival_policy_' --output-on-failure
```

`plots/` 中输出成批与稀疏两张对照图，各有 PNG、SVG，展示累计移交、实际收片进度和 Bank erase/program 时间区间。

6 项单元测试覆盖地址集合、批次时刻、稀疏对照、配置唯一差异、非法选择和覆盖保护。真实 CLI 集成测试检查：相同 trace／资源配置、实际策略次序、2μs 时 A 已收到半页、A 更早完成但整体完成时间相同、稀疏对照一致、重复运行确定性，以及损坏地址、时间、成员、Pending 积分和 Bank 记录的拒绝。工具不会为展示差异而另写组装或执行模型。
