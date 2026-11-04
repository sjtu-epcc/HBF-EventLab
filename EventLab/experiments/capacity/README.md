# 容量限制交互实验

本实验用确定性合成 workload 检查 **MOCS、Pending DLU／组装槽和 Bank 服务能力**的相互作用。它是机制诊断与实验基础设施，不是已校准的产品性能预测，也不替代真实应用 trace 的后续评估。

## 快速运行

先按项目[使用说明](../../docs/usage.md#构建)构建 `hbf_eventlab_cli`，建议扫描使用 Release。生成、扫描和分析只需要 Python 3.10 或更新版本的标准库。在仓库根目录执行：

```powershell
python experiments/capacity/run.py --cli build/Release/hbf_eventlab_cli.exe --plan experiments/capacity/plans/smoke.json --output out/capacity-smoke
```

Linux／单配置构建将 `--cli` 改为实际可执行路径，例如 `build/hbf_eventlab_cli`。输出目录必须尚不存在，脚本不会覆盖旧实验。中途终止的目录保留已有证据；重新运行请使用新目录。

`smoke.json` 共8组：8个独立块、两种顺序、Host打开上限4／5、Selected MOCS 256／512、Pending上限8、program 0.5μs。它用于检验实验链路和临界行为，不能据此概括整个参数空间。

`plans/pilot.json` 将Pending上限扩为2／8、program时长扩为0.5／8μs，共32组，适合先检查三个扫描维度。将上面命令的计划路径及输出目录替换即可。

可选静态绘图需要 `matplotlib`（其依赖包括 `numpy`）：

```powershell
python -m pip install -e ".[plots]"
python experiments/capacity/plot.py out/capacity-smoke --case fragment_round_robin-o5-c256-p8-t500000
```

热图分别显示原始逻辑DLU成功比例和组装上下文超时比例，色轴固定0—1。缺失、运行限额、停滞和脚本错误单元格标灰，不冒充0。资源图分别显示命令credit、Pending阶段和Bank忙闲。

完整建议矩阵有648组：

```powershell
python experiments/capacity/run.py --cli build/Release/hbf_eventlab_cli.exe --plan experiments/capacity/plans/round1.json --output out/capacity-round1
```

可以复制并修改计划JSON，先选少量切片。`--max-cases N`只运行计划前N组，manifest会明确标记扫描未完成；`--timeout-seconds`是每个模拟器进程的墙钟保护，不是模拟时间。默认120秒，较慢机器或大计划应提高。

## 固定条件与扫描变量

固定1 UCIe channel、1 die、1 Bank；每块8页，每页4096B。每个独立逻辑块仅写page 0，所有输入在Tick 0释放。每个64B片段是独立Global条目，仍通过现有Global地址入口、Host及链路进入设备。Host使用FCFS，写模式`fragment64`，关闭write burst；没有故障注入，Pending满与组装超时自动重试均设为0。

page 0会触发erase。因此本实验的Bank服务包含 **erase与program**，不能只用`Programming`计数估计Bank利用率。Core Channel保留当前理想模型；UCIe、RX、TX维持默认有限资源并保存在有效配置中。它们仍可能影响补片和credit释放，不声称实验中只有三个可能瓶颈。

| 计划字段 | 含义／建议值 |
|---|---|
| `dlus` | 固定原始DLU总数，也决定独立块数；默认64，共256KiB逻辑数据。支持1—1024。不是同时打开数量。 |
| `orders` | `dlu_major`：逐页发送64片后换页；`fragment_round_robin`：依次向各页发送第0片，再依次发送第1片……。 |
| `open_dlus` | Host每channel打开DLU上限；默认1、2、4、5、8、9、16、17、32。不保证实际打开这么多。 |
| `mocs_selected` | 设备Selected MOCS；默认256、512、1024。Supported固定为整个计划的最大Selected，不逐格变化。合法值沿用设备校验：1或256—16384间2的幂。 |
| `pending_limits` | 设备Pending上限；默认4、8、16、64，同时改变`上限×4096B`的组装payload预算。 |
| `program_ps` | 单Bank program服务时长；默认500000、2000000、8000000ps。Bank仍一次一个操作；改变服务时长不代表新增Bank并发能力。 |
| `erase_ps` | 固定erase时长，默认1000000ps。 |
| `assembly_timeout_ps` | 固定组装期限，默认10000000ps；页已Ready后不会因为介质排队再次触发组装timeout。 |
| `max_tick_ps`、`total_events` | 模拟运行限额；触限样本作为未完成记录，不当成普通失败或成功。 |

两种trace的地址、身份及数据量完全相同，只改变行顺序。第`b`块第`f`片的Global字节地址为`b×8×4096 + f×64`，命令大小64B，`content=1`。生成器检查全部页覆盖且没有重片。不能将它们称作实际应用trace。

可单独生成输入：

```powershell
python experiments/capacity/workload.py --dlus 64 --order fragment_round_robin --output out/rr64.txt
```

## 输出与指标口径

每次扫描保存：

- `manifest.json`：完整计划、完成程度、程序SHA256、Git状态、脚本与trace摘要、Python／平台信息。源码有未提交修改时明确记`dirty=true`，不能只凭commit复现该二进制。
- `workloads/`：实际运行的两份Global trace。
- `summary.csv`、`summary.json`：每格指标及模拟／进程退出信息，扫描过程中逐格更新。
- `cases/<名称>/input.ini`、`effective.ini`、`console.log`、`statistics.json`、`result.json`：输入参数、完整派生配置、原始输出和调用参数／文件校验摘要。
- 每格的`dlus.csv`、`contexts.csv`、`fragments.csv`、`pending.csv`、`bank.csv`和`resources.csv`：逻辑结果、原有上下文生命周期／片段进度、Pending时间线、真实Bank操作和统一时间轴上的资源曲线。

| 指标 | 分母／定义 |
|---|---|
| `dlu_success_ratio` | 全部64个原始逻辑片段最终成功的页数／计划原始页数。没有实际发送的页仍属于分母。 |
| `timeout_context_ratio` | `assembly_timeout`上下文数／实际打开的上下文数。分母0时为空值；0x4拒绝不会凭空产生上下文。 |
| `pending_limit_responses`、`pending_limit_affected_dlus` | 0x4命令响应数，以及这些命令涉及的不同原始页数。错误响应本身也占MOCS直到交付。 |
| `unhanded_fragments` | 从未移交链路的原始片段数；本轮无重试、无burst才可按总片段减attempt数计算。不能直接把它等同于丢包。 |
| `mocs_mean`、`mocs_full_ps` | 正式接受至响应交付的半开区间积分／观测时长，以及占满Selected的时长。 |
| `mocs_peak_tick_boundary` | 同Tick净变化后可重建的峰值；现有attempt时间戳没有事件序号，不能声称精确复原同Tick瞬时峰值。 |
| `pending_peak`、各阶段均值、`pending_full_ps` | 直接使用现有collector；`resources.csv`积分再与其复核。`pending.csv`保留同Tick事件sequence，便于查看瞬时次序。 |
| `bank_utilization` | `(erase忙时长 + program忙时长) / [0, statistics.at]`，本实验只有一个Bank。限额停止时，活动操作积分到真实观测时刻，完成时刻仍为空。 |
| `logical_completion_ps` | 全部原始逻辑工作已有最终结果的时刻；任何未完成则为空。失败提前结束不可当作性能提升。 |
| `simulation_end_ps`、`process_wall_seconds` | 前者为Runner停止时的模拟时刻；后者是启动及运行CLI进程的实际墙钟耗时，包含统计导出和日志写入，不含离线分析／画图。 |

`timeout_context_ratio`与`dlu_success_ratio`分母不同，**不是互补比例**。关闭重试后Host可能因0x4或0x5终止该逻辑块本内容版本的剩余工作，这个行为会影响成功率与后续负载。

关闭重试也不保证“一页最多一个上下文”：错误反馈返回前，已经进入链路／RX的旧片段仍需取得最终归属，可能在上一上下文超时后又建立新上下文。因此上下文数可能超过原始页数，必须保留generation及逐片段进度核查，不能据此误判发生了自动重试。

`run.py`退出0表示扫描／分析基础设施成功，允许存在有意制造的逻辑失败、Stalled或LimitReached；具体每格状态必须看summary。输入／模型错误、进程墙钟超时、JSON损坏或交叉核对失败记`harness_error`，脚本退出1，绝不将其填成0%成功或100%超时。

## 如何解释第一轮结果

先固定Pending上限高于Host打开上限，观察信用额度分散：256个命令名额足够容纳4个完整DLU，但在5个页间轮转时，可能每页只收到约51—52片，全部等待剩余片段。MOCS满只是背压；之后是否超时由真实组装期限与事件推进决定。逐页发送是相同数据量的对照。

再固定Selected较充裕，降低Pending上限，观察0x4、真实打开上下文和取消的剩余工作。最后改变program服务时长，看Ready等待、Bank忙时、credit驻留和其他Collecting上下文的超时；每一步仍需检查有限RX／TX和链路占用造成的间接影响。

这些是待用观测验证的机制假设，不应把`MOCS ≥ 64 × 打开上限`写成规范要求或充分必要前进性定理。全套实验只探索当前功能模型、固定Host策略与有限输入；比较Bank数量、多channel或真实trace需要另行保证容量预算、地址分布和输入覆盖可比。

## 验证

```powershell
python tests/test_capacity_experiment.py
python tests/test_capacity_experiment.py --cli build/Release/hbf_eventlab_cli.exe --output out/capacity-tests
ctest --test-dir build -C Release -R "statistics_bank_|experiment_capacity_" --output-on-failure
```

CMake找到Python 3.10+时自动注册两个实验CTest入口；没有Python时核心C++构建与测试仍可运行。实验集成测试覆盖256-credit的4／5页边界、顺序对照、Pending拒绝、运行限额、重复运行一致性、输出完整性、损坏记录拒绝和结果目录保护。Bank观察测试覆盖读／写／擦除、未完成快照与继续执行、并行Bank身份及重复／非法观察。统计不改变模拟事件和资源决定。

公共工具、实际配置核对、可选绘图依赖和测试开关见[实验基础设施](../README.md)。
