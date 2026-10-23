# HBF EventLab 使用说明

适用代码版本：0.5.0。以下命令均从仓库根目录执行。项目采用C++17与CMake，核心库无第三方运行依赖。已经验证的平台见[验证说明](verification.md#evidence)；参数用途见[参数说明](parameters.md)。

公开项目名为 **HBF EventLab**。C++ 命名空间为 `hbf_eventlab`，公共头文件位于 `include/hbf_eventlab/`，CMake 链接目标为 `HBFEventLab::core`，CLI 为 `hbf_eventlab_cli`。workload 必须使用 `HBF_EVENTLAB_WORKLOAD 1` 格式头；格式名和版本均严格校验。

## 构建

完整测试需要 Python 3.10+。`HBF_EVENTLAB_EXPERIMENT_TESTS` 默认 `ON`：开启测试但找不到解释器时，CMake 会报错，不再静默省略实验测试。可用 `-DPython3_EXECUTABLE=C:/path/to/python.exe` 指定解释器。只验证 C++ 时显式传入 `-DHBF_EVENTLAB_EXPERIMENT_TESTS=OFF`，此时不应报告为完整回归；`-DBUILD_TESTING=OFF` 的纯构建不要求 Python。实验运行与测试无第三方 Python 运行依赖，绘图依赖按需通过 `python -m pip install -e ".[plots]"` 安装，见[实验公共基础设施](../experiments/README.md)。

Windows需安装Visual Studio 2022 C++工具和CMake 3.20或更新版本。推荐执行：

```powershell
.\build.ps1 -Configuration Debug
.\build.ps1 -Configuration Release
```

脚本构建、执行CTest，并运行组件示例、Device示例与正常Global CLI。`-SkipTests`跳过CTest，但仍运行示例。

也可以直接使用CMake（以下与脚本同为x64多配置构建；已有目录使用不同生成器时请另建目录）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Linux 需要支持 C++17 的编译器、CMake 3.20 或更新版本和 Ninja；Ubuntu / Debian 的安装命令见 [README 快速开始](../README.md#quick-start)。Ninja 单配置构建使用以下命令：

```sh
cmake -S . -B build-linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-linux-release --parallel "$(nproc)"
ctest --test-dir build-linux-release --output-on-failure --parallel "$(nproc)"
./build-linux-release/hbf_eventlab_cli examples/workloads/normal.txt
./build-linux-release/hbf_eventlab_cli examples/workloads/recovery.txt --open 4 --fail-sequence 2
```

Debug 使用独立的 `build-linux-debug` 目录和 `-DCMAKE_BUILD_TYPE=Debug`；完整回归命令见 [贡献指南](../CONTRIBUTING.md#testing-and-verification)。

## 运行示例

```powershell
# 写入并有序读回
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt

# 显式4KiB local写burst
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --burst

# page 2失败一次，由Host自动恢复
.\build\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --open 4 --fail-sequence 2

# 恢复又在page 1失败，未完成终点仍保留到page 2
.\build\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --open 4 --fail-sequence 2,1

# 预期退出码2：耗尽恢复预算
.\build\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --persistent-page 0

# 预期退出码2：恢复数据缺失
.\build\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --fail-sequence 0 --retention 0
```

正式Global入口为 `hbf_eventlab_cli`。`hbf_eventlab_device_demo`展示独立Device事务驱动；`hbf_eventlab_demo`展示低层组件操作，不能代替Host闭环验证。单配置构建的可执行文件位于构建目录根部，不含`Release/`子目录。

## CLI参数

| 参数 | 默认值与含义 |
|---|---|
| 第一个位置参数 | workload文件；运行时必需；单独使用 `--help` 可查看用法。 |
| `--config FILE` | 加载一个 INI 配置文件；未指定时使用功能默认配置。 |
| `--set section.key=value` | 覆盖任意已注册参数；可重复使用。完整字段、单位和约束见[参数说明](parameters.md)。 |
| `--dump-config FILE` | 导出包含所有默认值与显式覆盖的有效配置，可再次加载。 |
| `--speed-grade N` | 默认1；选择1/2/3，派生带宽、最大模块数和最大堆叠层数。 |
| `--mocs-supported N` | 默认256；声明设备支持的MOCS上限。 |
| `--pending N` | 默认8；每channel的Pending写DLU上限，同时决定组装payload预算。 |
| `--channels N` | 默认2个UCIe channel；静态几何仍校验合法范围。 |
| `--interleave bytes` | 默认64B，Global交织粒度。 |
| `--base bytes` | 默认0，Host全局地址起点；输入地址包含此base。 |
| `--burst` | 默认关闭；显式将完整local DLU组为一次4KiB写事务。 |
| `--regular` | 默认关闭；开启后全部读用Regular，默认则为每Global每channel尾Batch。 |
| `--open N` | 默认每channel两个Host打开DLU名额，必须为正。 |
| `--replays N` | 默认每恢复任务最多三遍；0禁用恢复尝试。 |
| `--pending-retries N` | 默认3；每个发送单元收到0x4后允许的额外命令尝试次数，初始发送不计；0关闭此类自动重试。 |
| `--timeout-retries N` | 默认3；同一内容、页及当前pass内允许的额外整页组装轮数，初始组装不计；0关闭此类自动重试。 |
| `--retry-delay N` | 默认1000000 Tick（当前为1µs）；从0x4响应计起的兜底等待间隔。容量相关完成反馈可提前唤醒；启用Pending重试时必须为正。 |
| `--retention N` | 默认65536个64B符号片段；0关闭恢复数据保留。 |
| `--mocs N` | 默认256；选定MOCS。Supported与Selected均须为1或256—16384内的2的幂，且Selected≤Supported。 |
| `--fail-sequence p,q` | 默认无；按序在指定页注入一次可重用program失败。 |
| `--persistent-page p` | 默认无；该页每次program均失败。 |
| `--max-tick N` | 默认1000000000000，绝对模拟时间上限，单位ps。 |
| `--max-events N` | 默认10000000，累计callback上限；0关闭该项限额。 |
| `--trace` | 默认关闭；输出Host/Device事件及命令身份。 |
| `--stats-json FILE` | 默认关闭；导出统一统计汇总、Global/逻辑片段/命令尝试明细、Pending曲线和DLU上下文历史。父目录须已存在；禁止覆盖输入workload。 |

覆盖顺序为内建默认值 → 配置文件 → 显式CLI选项；`--config`的位置不影响优先级，同一字段的多个CLI覆盖按从左到右最后一次生效。未知字段／策略及非法组合在运行前报错。参数调整不需要重新编译。

```powershell
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --config configs\functional.ini --mocs-supported 1024 --mocs 512 --dump-config effective.ini
```

故障注入默认作用于逻辑channel 0 / die 0 / bank 0 / block 0；通过`faults`小节可改变目标。页号越界、同时指定非空失败序列和持续失败页均报输入错误。Host看不到注入器。`--help`打印用法并以0退出，无参数运行以5退出。

默认功能几何为每channel 1 die / 1 bank、每块8页、每bank 4个可见块与1个spare；2channel总逻辑容量256KiB。它不是产品配置，几何、Pending、介质时延、窗口、缓冲及Core路由均可通过同一配置入口修改。Grade 1最多支持8个模块；16channel示例需选择Grade 2或3。配置在一次模拟内冻结。

## 有限 workload 格式 v1

~~~text
HBF_EVENTLAB_WORKLOAD 1
WRITE 1 0 4096 content=1
WRITE 2 4096 4096 content=1
READ 3 0 8192 after=1,2 expect=1 group=7
~~~

- 必需字段：操作、唯一 Global ID、**Global 字节地址**、字节长度。支持十进制及 0x 十六进制；# 后为注释。
- WRITE 必须有 content：逻辑块范围内不可变的内容身份。多个 Global 条目补同一 DLU 使用相同 content；重写新内容使用新 content。它不是设备 epoch，也不是 PEC。
- READ 的 expect 可选，用于验证 DataToken；省略则只检查 ReadStatus。group=none 或省略表示无组序；整数表示 channel 内同组返回顺序，不模拟真实 AXI ID 字段。
- after=1,2 仅用于 READ：这些 Global 工作最终成功后才发送。父工作最终失败时，尚未发送的依赖读以 DependencyFailed 结束，不制造 Device completion。
- at=Tick 是最早移交时刻，默认 0；不表示已到达 RX 或正式接受。
- WRITE 的任何额外依赖均在 load 阶段报 LIMIT-WRITE-001。文件行次序不隐含“上一条已完成”。
- 每条范围的长度非零、64B 对齐、溢出、profile 容量、依赖存在与无环均先检查；失败不会向 Device 发送部分输入。允许仅含格式头、没有请求的空 workload。
- LIMIT-CONTENT-001：同块内容身份不能 A→B→A 复用；LIMIT-CONTENT-002：同内容中的同一 64B 写片段不能重复。这是 workload 能力限制，**不是 OCP overlap 规则**；设备层仍按 overlap 规则判断重复写。
- 同块多个内容版本按声明顺序运行；旧版本失败则后续版本标记 PriorContentFailed。本版没有任意依赖图下的新旧内容读快照语义。

默认拆成 64B Read/Write。显式 --burst 将完整的同内容、同 local DLU 的 64 个片段组成一次写事务；可跨 Global 条目，但全部可释放后才能发送。不足整页则报 BURST-INCOMPLETE，不自动补零，不生成读 burst。

DataToken 由内容身份和原 Global 地址确定，仅验证符号身份，不是真实字节或无碰撞数据完整性证明。恢复从有限保留区取原 Global 地址和 token，再经同一 interleaver/splitter 转换，不从物理地址反推数据。

<a id="host-send-policy"></a>

## Host 基线与可替换边界

每 channel 一个待发集合；`IHostSendPolicy` 只在 `HostDriver` 过滤的合法候选中选择。`host.send_policy` 支持两种策略：

| 策略 | 选择顺序 |
|---|---|
| `fcfs`（默认） | 当前合法候选中本次入队最早者优先。受阻者允许被其他合法候选越过，不强制队首阻塞。 |
| `fill_open` | 优先补齐已打开 DLU，其次 replay，再按本次入队顺序。 |

普通发送单元按 `at` 释放时间进入候选管理，同 Tick 按输入行／行内片段次序登记；Global ID 数值不表示优先级。显式 burst 等全部贡献片段释放后才入队，位置取最后一个贡献片段，避免预加载时的对象构造顺序改变发送次序。合法性检查、版本依赖、同组读及恢复屏障仍优先于策略，等待这些条件时保留原入队序号。

`NotTaken` 不改变入队序号，也不增加命令尝试次数；真正的错误重试在旧尝试终结且满足重试门控后取得新序号，排到已有候选之后。重试保留 `SendUnitId` 和原逻辑工作身份，使用新的 `CommandId`。新生成的 replay 单元按生成时刻入队，恢复后重发的失败后继也重新排队。同 Tick 的后续重试按实际事件处理次序登记，不通过增加 Tick 制造排序。两种策略共用这些入队规则；`fill_open` 仍可因补齐／replay 优先级选择后入队者。

例如，固定输入和其他参数，对比两种发送策略并导出有效配置：

```powershell
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --set host.send_policy=fcfs --dump-config build\fcfs.ini
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --set host.send_policy=fill_open --dump-config build\fill-open.ini
```

这些策略不保证超载 workload 能成功。FCFS 可能同时留下更多未收齐 DLU，结果还取决于 MOCS、Pending 上限、Host 打开上限、数据到达次序和重试预算。

打开 DLU 上限默认每 channel 2；Host 从首条 Taken 到该 DLU 已移交命令全部收到最终响应，保守占用自己的打开名额。这不是 Device Pending 数。MOCS=1 不能同时容纳 64 个等待整页完成的写命令，该配置允许真实走到 timeout；显式 burst 可以前进。

默认每 Global 读范围在各 channel 最后一条为 Batch，其余 Regular；尾 B 等同范围前序命令正式接受后才发送。--regular 全部 Regular，由主动窗口 timer 推进。同组号的不同 channel 不构成全局排序域。

公开接口边界：

1. NotTaken：源保留待发单元，按公开 WaitKey 定向订阅；没有未结尝试、没有 completion。
2. Taken：返回 TransferAttemptId，Host 登记未结 CommandId；后续 Link/RX 重试归 Device。
3. AcceptedNotice：异步观察正式接受，更新 Host 前页的暂定发送进度和 replay 遍数；错误命令也有此通知，不保证数据已进入Assembly，不是额外命令或 completion。
4. DeliveredResponse：恰好一次终结尝试。Host 消费反馈前仍可保留未结记录，此时设备 credit 可能已经释放。

## 写重试与自动恢复

### Pending 满、组装超时与关联写序错误

资源重试由 `HostDriver` 的页级修复状态管理，与program-fail的block replay分开。Host复用原发送单元和逻辑内容，每次重试分配新 `CommandId`；旧命令必须先取得最终响应。

| 写响应 | Host默认行为 |
|---|---|
| `0x4 PendingDluLimit` | 撤销失败命令对应的暂定bitmap位，只重发该命令。保留其他有效片段，继续补齐最早受影响页；不等整个block排空，以免等待正在缺片的DLU完成。 |
| `0x5 AssemblyTimeout` | 暂停该页及后继页的新发送，等待该页同一pass内所有已移交尝试终结，包括Link/Ingress中的旧片段。确认完整64片覆盖、已发送数据仍在Host保留区，且未来片段均达到`at`边界，再重置整页bitmap并组织完整4KiB重发。多条timeout响应只驱动一轮修复。 |
| `0x6 / additional_info=1` | 先等待可能迟到的前页根因。能关联到同内容、同pass的前页0x4/0x5时，按页修复；被拒绝后继的重发保守地等待前页全部成功。已经有效接收的后继片段继续原生命周期。无法建立关联时明确失败。 |

正常写仍允许前页全部收到接受通知后发送后页；bitmap只控制Host发送，不替代Device顺序检查。错误修复期间使用更保守的门控。设置 `--pending-retries 0 --timeout-retries 0` 可关闭这两类自动重试；`--replays` 单独控制program-fail恢复。

0x4等待只使用Host能观察到的同channel写完成反馈，并有正时间间隔的timer兜底，不读取Device的Pending表。完成反馈只是重查提示，不保证名额仍可用；兜底探测也可能再次得到0x4。当前等待策略和默认次数属于模型选择，没有硬件校准依据。

| Host诊断 | 含义 |
|---|---|
| `PendingRetryBudgetExhausted` | 某发送单元的0x4额外尝试用尽。 |
| `AssemblyRetryBudgetExhausted` | 当前页、当前pass的额外组装轮数用尽。 |
| `WriteStatus:5 MissingRetryData` | 输入未覆盖整页，或已发送片段的恢复数据已不在保留区。 |
| `UnexplainedWriteOrderViolation` | 前页在途尝试已终结，仍无法将0x6/info1关联到支持的资源修复原因。 |

这些是Host逻辑失败原因，不是新增OCP状态码。关闭对应重试时保留 `WriteStatus:4` / `WriteStatus:5` 的终止行为。失败结束相关内容版本的未完成工作并继续排空旧尝试；不会凭空补数据，也不保证持续超载的配置能成功。

例如，对比相同输入的发送策略时可以显式记录重试配置：

```powershell
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --pending-retries 3 --timeout-retries 3 --retry-delay 1000000
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --pending-retries 0 --timeout-retries 0
```

### Program-fail replay

Host RecoveryManager 与设备 ReplayState 分属两侧：前者组织重发，后者约束介质合法性。Host 不查询后者。

- 0x7 用自身原命令地址确定失败页；0x6/info2 可先到，只冻结旧尝试，不猜测根因页。
- 每遍失败冻结同块已经 Taken、尚未被 Host 处理最终响应的有限 ID 集合，包含旧读、Link、Ingress、接受后的操作。之后新读不追加进该集合。
- 等旧集合排空且根因已知，再核验完整 0..终点数据，生成新 page-0。重复失败合并同一任务，最远终点只增不减。
- 默认最多 3 遍 replay。初始普通写不计；每遍第一条 page-0 正式接受时计一次，不按 64 个响应累加，也不因再次失败重置。
- replay 按页：持续发送整页片段，再等该页全部响应成功才发下一页。此保守基线降低并行度，但简化顺序和排空证明。
- 前缀验证后固定保留到恢复完成/终止；其他块不能逐出。其他保留项按 FIFO 淘汰；全被固定时不保留新普通写的数据，但不阻断写入，后续可能报告数据不足。
- 终点响应全部成功后 Host 才确认恢复完成；重发先前失败的后继普通页，分配新 CommandId。
- MissingRecoveryData / RecoveryBudgetExhausted 结束相关逻辑工作及依赖；旧命令继续排空。历史成功不改写，设备未解除的恢复限制保留。
- 无依赖普通读照常发送，按 Pending/0xA/0x2 等得到一次结果；不自动循环重试。
- replay中遇到0x4/0x5可在当前pass内修复，不因此增加block恢复任务或replay遍数；真正再次program-fail才进入原有的下一遍恢复流程。block恢复正在排空旧尝试时，旧资源错误由该恢复流程统一处理。

### 写重试统计

CLI的 `HOST` 行同时报告命令尝试和逻辑工作结果。计数单位如下：

| 字段 | 计数单位与边界 |
|---|---|
| `pending_limit_errors` / `assembly_timeout_errors` | 收到的0x4 / 0x5命令响应条数，包含关闭重试或逻辑版本已失败后排空的响应。不是失败DLU数。 |
| `pending_retry_commands` | 因0x4再次被端口Taken的命令尝试数；burst仍计一条。 |
| `timeout_retry_rounds` | 通过排空、覆盖与数据检查后，开始准备的整页重试轮数；不是已成功program页数。 |
| `timeout_retry_commands` | 该轮中再次Taken、且以前确实发送过的命令数；原先未发送的未来补片不计重传。 |
| `order_retry_commands` | 因关联0x6/info1修复而再次Taken的命令数。 |
| `attempt_errors` | 所有失败尝试的累计数；不因之后恢复成功而减去。 |
| `logical_success` / `logical_failed` | 最终成功/失败的原始64B逻辑片段数；重试不会复制逻辑工作。 |

例如64条timeout响应可能对应一轮整页重发；发生尝试错误的Global条目仍可能最终成功。三类重发命令计数按本次重发原因分类；replay中的资源重试也属于 `replay_commands`，因此不能将它与上述重试计数相加当作互斥总数。实验比较应同时保存配置、尝试错误、逻辑结果与排空状态。

## 统一统计

```powershell
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --stats-json build\normal-statistics.json
.\build\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --fail-sequence 2 --stats-json build\recovery-statistics.json
```

`IStatisticsSink`只接收观察事件；`StatisticsCollector`提供有限workload的精确记录、只读查询及JSON。Host和Device在load/首次移交之前挂接同一个collector，其生命周期应覆盖两侧。一个collector对应一次Host/Device运行；限额中断后继续同一次运行时沿用它。独立Device也可单独挂接，此时没有Global/逻辑记录；替代Host端口用于完整统计时，应提供相同的Device侧统计事件。

```cpp
StatisticsCollector statistics; // 定义在 Host / Device 之前
// 构造 events、device、interleaver、host、runner ...
device.set_statistics_sink(statistics);
host.set_statistics_sink(statistics);
host.load(workload);
host.start();
const auto report = runner.run();
const auto summary = statistics.summarize(report.at);
// globals() / logical_work() / attempts() 提供请求明细。
// pending_timeline() / dlu_contexts() 提供占用变化及组装上下文明细。
```

五个时刻均为模拟事件时间，“释放”指workload释放，不是资源释放：

| 字段 | 边界 |
|---|---|
| `globals[].released` | 原始Global的`at`释放边界，逻辑子工作继承它；重试不重置。`scheduled_release`保留输入时刻，尚未释放或提前取消时`released`为空。依赖等待也计入释放后的时间。 |
| `attempts[].handoff` | 端口Taken，预留RX并开始请求传输，尚未消耗MOCS。NotTaken不产生尝试记录。 |
| `attempts[].rx_arrival` | 请求序列化及传播结束，到达RX。 |
| `attempts[].accepted` | 正式接受、占用MOCS并释放RX。接受后报错的命令也有此时刻，不保证写数据进入Assembly。 |
| `attempts[].delivered` | 返回传输结束、credit释放。取Device事件时刻，不取后续Host回调或导出时刻。 |

另外保存`queued_at`（本次发送单元入队边界，重试更新）、`assessed`（Host处理响应）、`finished`（逻辑最终归属）。初始跨Global写burst的`queued_at`取其全部贡献片段均到达释放时刻的边界；各Global仍保留各自释放时刻。

延迟键按read/write、层次、success/failed/pending分组，例如`write.attempt.success.accepted_response`：

| 指标后缀 | 定义与样本单位 |
|---|---|
| `global.*.end_to_end` | Global全部逻辑子工作最终结束的时刻－原始释放。不取平均，也不因第一个错误提前完成。 |
| `logical.*.end_to_end` | 原始64B逻辑工作最终结束－原始释放；包含相关重试及恢复等待。 |
| `logical.*.initial_host_wait` | 该逻辑工作第一次实际移交－原始释放；尚无移交则没有样本。 |
| `attempt.*.queue_wait` | 本次移交－本次`queued_at`。 |
| `attempt.*.request_link` | RX到达－移交，包含序列化和传播。 |
| `attempt.*.ingress_wait` | 正式接受－RX到达。 |
| `attempt.*.accepted_response` | 交付－正式接受，即一条命令的credit持有时长。包括组装、调度、介质和响应排队/传输，不等同于NAND时延。 |
| `attempt.*.handoff_response` | 交付－移交。 |

每个分布给出`count/min/max/mean/p50/p95/p99`，分位数用nearest-rank；没有样本的键不生成。未交付尝试只有已经结束的分段，归入pending，不把当前等待年龄当作完整响应延迟。零延迟有效；缺失时刻用JSON `null`。

JSON schema为2，时间单位为Tick（CLI中1ps）。顶层包含`summary`、`globals`、`logical_work`、`attempts`、`pending_channels`、`pending_timeline`和`dlu_contexts`；原有请求统计字段保留。64B逻辑工作与真实移交事务分别计数：完整4KiB burst只计一条尝试，但可关联多个Global的64个片段；replay通过channel/local地址/content关联原始工作，不创建新的逻辑分母，不重复完成历史成功。明细的`work_ids`、`unit`、`unit_attempt`和`replay_pass`支持继续细分。`replay_attempts`与`retried_unit_attempts`可能交叉，不能相加。

尝试success/failed按Device协议状态划分；`responses`以`op + status`为键，读写同数值状态不合并。Host的DataToken检查失败另计`host_verification_failures`，因此协议成功不必然表示逻辑成功。逻辑记录保留失败`reason`。

`Stalled`、`LimitReached`及运行中的模型诊断也导出已观察记录与停止原因。未完成项保留pending；尚未释放就取消的工作计入failed及其`cancelled_before_release`子集，不计算负的端到端延迟。输入校验或输出文件创建失败时没有有效运行统计。时刻和ID应以64位整数读取，避免JavaScript浮点数在大值处丢精度。

默认collector空间为O(原始工作数＋命令尝试数＋组装状态变更数＋介质操作数)，导出和分位数计算有宿主程序开销；这些记录不代表模拟硬件缓冲，不增加事件或Tick。未开启时不保存统计历史。Bank操作区间由统一接口导出，可离线计算忙闲曲线与利用率。核心JSON不含宿主程序墙钟耗时；容量实验脚本另行记录CLI进程耗时。所有模拟时间结果仍是未校准的功能模型结果。

同一Global可能跨多个块，部分子工作在释放前被取消，其余子工作仍在以后释放、完成；因此`logical_work`单独保存`cancelled_before_release`和`released`，不能仅从父Global的最终状态推断。Global仍等剩余子工作结束后取得最终结果。

实验记录还应保存输入文件、`--dump-config`导出的有效配置及CLI的CONFIG/STOP行；统计JSON不内含完整设备与Host配置。

### Pending曲线与DLU上下文

开启同一个`--stats-json`选项即可记录以下内容，无需额外采样参数。数据来自`WriteAssembly`已经提交的状态变化，片段数量直接取现有`received.count()`；统计层不维护第二份bitmap，不决定何时收齐、发射或超时。

| 每channel计数 | 口径 |
|---|---|
| `total` | 当前真实Pending上下文数，占用每channel的Pending名额及组装槽。 |
| `collecting` | `Collecting`，还未收到全部64个64B片段。 |
| `ready_waiting` | `Ready`与`ProgramQueued`之和；数据已收齐，但尚未开始program。可能在等待erase、前页成功、Bank或调度，不能全部解释为Bank排队。 |
| `programming` | 已开始program、尚未终结的DLU数；不包含erase或read占用，因此不等于Bank利用率。 |

始终满足`total = collecting + ready_waiting + programming`。`pending_channels[]`给出各channel的`limit`、观测区间`observed_from/observed_until`、`current`、各维独立`peak`、`area_context_ticks`和`time_weighted_mean`，以及`at_limit_ticks`、累计`opened/retired`。均值等于占用积分除以观测时长，包括初始空闲和最后一条变化记录至导出时刻的尾段；零时长时均值为0。不同维度的峰值可能发生在不同时刻，不能相加推导总峰值。

`pending_timeline[]`每条包含`at`、`sequence`、`channel`与变化**之后**的`counts`。每channel从挂接时的零样本开始，只在四项计数变化时追加；普通补片的数量变化保存在上下文`progress`中。重建曲线时：

1. 按channel筛选，再按`(at, sequence)`排序。
2. 每条计数保持到下一条样本，形成右连续阶梯曲线；相同Tick的中间状态持续时间为0，仍可用sequence还原次序。
3. 最后一条延伸到JSON顶层`at`；不要延伸到配置的时间限制，因为Runner可能停在限制之前的最后一个事件。
4. 对每段计算`count × ΔTick`并求和，即可复核占用积分。各channel曲线要在统一时间轴上对齐后才能相加。

`dlu_contexts[]`以`(channel, local_dlu, generation)`唯一标识一次组装上下文。`generation`复用现有上下文身份，同一地址超时后重试或replay会形成不同记录，不能只按地址去重。

| 字段 | 含义 |
|---|---|
| `received_fragments`、`progress[]` | 当前／终结时的唯一已收片段数，以及每次补片、阶段变化和终结的`at/sequence/change/phase/received_fragments/command`。完整页为64；4KiB burst仍产生64个片段进度。 |
| `opened_at`、`deadline` | 首个有效片段建立上下文的时刻及组装期限；收齐后历史deadline仍保留，不表示之后会超时。 |
| `ready_at`、`program_started_at`、`retired_at` | 数据收齐、实际开始program、上下文和组装槽释放；尚未发生则为`null`。 |
| `phase`、`active` | 当前阶段，或已终结上下文的最后存活阶段；`active=false`由真实终结决定。 |
| `termination`、`status`、`additional_info` | `program_success`、`program_fail`、`prior_program_fail`（0x6/info2）或`assembly_timeout`；活动上下文三项均为`null`。 |
| `assembly_ticks` | `ready_at - opened_at`，只有收齐才有完整样本。 |
| `ready_wait_ticks` | `program_started_at - ready_at`，不是纯Bank等待时间。 |
| `program_ticks` | `retired_at - program_started_at`，只对真实开始且终结的program有值。 |
| `residence_ticks`、`observed_residence_ticks` | 前者是终结减打开，未终结为`null`；后者允许活动上下文计算截至顶层`at`的已观测驻留时长。 |
| `members[]` | 真正把片段加入此上下文的`command/attempt`。关联到`attempts[].work_ids`再到`logical_work`即可追溯Global、内容和重试；单独观察低层WritePath而没有传输尝试时，`attempt=null`。 |

`summary.latency`中的`dlu_context.<终结原因或active>.assembly/ready_wait/program/residence`按上下文统计，只纳入起止时刻都存在的阶段；不会把未完成等待作为完成延迟。0x4、overlap等拒绝不建立虚假上下文，也不算旧上下文的终结。`retired`在这里仅指Pending释放，不是物理容量退役，也不是响应交付或MOCS释放。

实验分母需明确：`opened`统计上下文尝试，包括超时重试和replay；未能建立上下文的0x4仍只能从命令响应统计中看到。逻辑成功比例应使用原始逻辑工作及其最终结果，不能用上下文成功数替代。在限额停止时，活动上下文保留，报告已终结样本和未完成数量；析构清理不伪造终结。

时刻、计数、generation和sequence以整数导出；积分、均值和分位数均值使用浮点汇总，极大运行可能产生舍入。需要大整数精确积分时，可从整数时间线离线计算。

### Bank操作观察与容量实验

同一个`--stats-json`输出增加`bank_operations[]`：`ticket`、`kind`（`sense/program/erase`）、物理`channel/die/bank/block/page`、`issued_at`、`completed_at`和`observed_busy_ticks`。ticket来自真实`DeviceRuntimeState`，只在发射提交成功后登记；候选查询、Bank预留后的回滚、Cache命中不虚构介质操作。结束在介质完成且Bank释放时记录，不等Host收到响应。

每个Bank的操作区间为`[issued_at, completed_at)`；活动操作的`completed_at=null`，仅观测到JSON顶层`at`。按物理Bank分别累积区间长度，再除以明确的观测时长，即可得到利用率；跨Bank允许并行，不能把忙时长直接相加后仍除以单Bank时长。此字段为schema 2的向后兼容扩展；旧schema 2文件可能没有该数组，分析程序应显式检查而非假定零占用。

[容量实验工具](../experiments/capacity/README.md)提供单channel/die/Bank合成trace、配置矩阵、逐DLU结果及资源曲线。其生成与分析只依赖Python标准库，绘图可选matplotlib。扫描分别报告逻辑失败、限额停止和工具错误，并保存完整有效配置；不从AcceptedNotice推断已收片段，也不重新实现组装逻辑。

[交织与到达间隔实验](../experiments/interleave_arrival/README.md)提供多channel的连续Global写入、`interleave_bytes × at间隔`扫描、逐页计划释放与实际组装对照，以及逐channel资源曲线。相同输入大小和间隔共用相同trace；实际超时上下文与因同块取消而失败的原始DLU分别统计。`at`仍表示Host输入释放，不是Device接受时刻。

[到达形态与Host策略最小例子](../experiments/arrival_policy/README.md)固定两个独立block的首页，对同一份成批trace分别运行`fcfs`和`fill_open`，另用稀疏到达作对照。工具核对输入及资源配置一致，记录真实移交次序、组装和Bank执行；更早完成某个DLU与缩短整份workload时间分别报告。

### 读窗口与实际服务观察

统一JSON增加`read_paths[]`和`read_window_releases[]`，仍为schema 2的增量字段；旧文件可能缺少，消费者必须检查存在性。

`read_paths`按`attempt`关联已有命令尝试：`window_entered_at`、`window_left_at`、`release_reason`、`service`（`cache/sense`）、`service_at`、`bank_ticket`。窗口退出原因为空但退出时刻非空，表示Cache／终结条件使其移出，并非窗口集合释放。早期Cache命中不进入窗口；Pending转发、立即错误和未推进到服务的读可能没有Cache/Sense记录，不能把空值当作命中或失败。最终结果仍由attempt响应状态与Host逻辑结果解释。

`read_window_releases`记录channel、成员attempt、首成员进入时刻、计划deadline、实际释放时刻和原因（`batch/timer/threshold/immediate/policy`）。内建策略同次多个条件满足时，诊断优先级为Batch、零等待、数量阈值、timer；自定义策略可使用`policy`，不影响实际资格判断。每个Bank的sense记录可通过`bank_ticket`与读关联。服务时刻为Cache取得结果保护或sense实际发射，均不等于最终响应交付。

执行[读窗口与NAND调度最小实验](../experiments/read_scheduling/README.md)可比较两种NAND策略及窗口释放边界。研究Regular累积时使用`host.read_hint_mode=all_regular`；默认`tail_batch`会将每条单独64B Global读标为Batch。

## 运行器与参数

参数键、默认值、单位、合法范围、零值语义和依据集中维护在[参数说明](parameters.md)。Tick固定为1ps；默认NAND／Cache时延及RX／TX深度是未校准模型假设。Core Channel当前只表达拓扑，采用无限带宽、零延迟模型；修改参数不代表引入新的介质并发机制。

CLI 打印配置、Global 结果、尝试错误、恢复次数、设备操作数和活动摘要。总事件数从 EventQueue 初始化起累计；恢复运行应提高总预算，不重新计数。时间限制检查下一个事件，不强行推进到限制时刻。

| 停止状态 | 含义 | CLI exit |
|---|---|---:|
| Completed + Success | 工作成功，双方瞬态排空 | 0 |
| Completed + Failed | 工作已有失败归属，瞬态正常排空 | 2 |
| Stalled | 尚有工作，无有效未来推进事件 | 3 |
| LimitReached | 保留现场，触及时间/事件/轮次限制 | 4 |
| ModelError / 输入诊断 | 超出支持子集，如 LIMIT-READ-001 | 5 |
| ImplementationError | 内部不变量、策略返回或回调身份异常 | 6 |

持久 NAND、映射、PEC、无活动引用的 Cache、放弃恢复后的设备限制不要求清空。未来 timeout、窗口、介质完成和 release 事件仍直接跳时执行；被动订阅不会凭空产生进展。

## 支持边界

完整响应子集、输入限制和未实现功能见[支持范围与限制](scope_and_limitations.md)。验证矩阵、结果及复现步骤见[验证说明](verification.md)。
