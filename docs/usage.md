# HBF EventLab 使用说明

适用代码版本：0.5.0。以下命令均从仓库根目录执行。项目采用C++17与CMake，核心库无第三方运行依赖。已经验证的平台见[验证说明](verification.md#evidence)；参数用途见[设计决策](design_decisions.md#5-参数与模型假设)。

公开项目名为 **HBF EventLab**。C++ 命名空间为 `hbf_eventlab`，公共头文件位于 `include/hbf_eventlab/`，CMake 链接目标为 `HBFEventLab::core`，CLI 为 `hbf_eventlab_cli`。workload 必须使用 `HBF_EVENTLAB_WORKLOAD 1` 格式头；格式名和版本均严格校验。

## 构建

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
| 第一个位置参数 | workload文件；必需。 |
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
| `--mocs N` | 默认256；本CLI capability为256，所以可选1或256。 |
| `--fail-sequence p,q` | 默认无；按序在指定页注入一次可重用program失败。 |
| `--persistent-page p` | 默认无；该页每次program均失败。 |
| `--max-tick N` | 默认1000000000000，绝对模拟时间上限，单位ps。 |
| `--max-events N` | 默认10000000，累计callback上限；0关闭该项限额。 |
| `--trace` | 默认关闭；输出Host/Device事件及命令身份。 |

CLI故障注入仅作用于逻辑channel 0 / die 0 / bank 0 / block 0；序列元素为页号，越界先报输入错误。Host看不到注入器。所有未声明参数均报错；当前无独立`--help`选项，无参数运行会打印用法并以5退出。

CLI使用每channel 1 die / 1 bank、每块8页、每bank 4个可见块与1个spare。默认2channel总逻辑容量256KiB；这是功能几何，不是产品配置。API可以构造其他合法几何，测试包含16channel/64B交织。

除上表可选项外，Pending上限、时延、读窗口、缓冲深度和Core路由通过 `StaticDeviceDescription` / `ExecutionConfiguration` 配置；不要给CLI传入尚未实现的参数。配置在一次模拟内冻结。

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

## Host 基线与可替换边界

每 channel 一个待发集合；IHostSendPolicy 只在 HostDriver 过滤的合法候选中选择。默认 FillOpenDluPolicy 优先级：补齐已打开 DLU、replay、稳定 SendUnitId。策略不能取消接受顺序、版本依赖或恢复屏障。

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

## 运行器与参数

| 参数 | 默认值 | 来源性质 |
|---|---:|---|
| Tick | 1 ps | 项目分辨率 |
| sense / program / erase | 100000 / 500000 / 1000000 ps | 功能演示值，未校准 |
| Cache access | 100 ps | 模型假定 |
| 每 Bank Cache | 两个 4KiB 槽 | 规范相关组织；具体槽与保护机制为模型选择 |
| UCIe 有效带宽 | 每方向 48 GB/s | 依据 §5.1.2 档位，完整事务成本仍是模型 |
| 控制服务量 / 固定传播 | 请求/响应各16 B、各100 ps | 模型假定，不是逐拍 AXI |
| RX / TX | 每 channel 16项+16384B / 8项+512B | 模型容量参数 |
| Core Channel | 拓扑存在，无限带宽、零延迟 | 模型假设 |
| 读窗口 | 10000 ps 或32条，Batch即释放 | 数值为策略参数 |
| MOCS / Pending | CLI 256 / 8 | 功能配置，仍经过能力校验 |
| Assembly timeout | 10000000 ps | 功能演示值 |
| Host 打开 DLU / 保留区 | 每channel 2 / 65536×64B=4MiB | Host 策略，符号容量不等于 C++ 内存 |
| replay 预算 | 每任务3遍 | 工程保护，不是规范次数 |
| Pending 重试预算 / 兜底间隔 | 每发送单元额外3次 / 1000000 ps | Host策略；完成反馈可提前触发重查 |
| timeout 重试预算 | 每内容、页、pass额外3轮 | Host策略；与replay遍数分开 |
| 输入上限 | 1048576个64B逻辑片段 | 有限输入保护 |
| 模拟时间 / 总callbacks | 1 s / 10000000 | 工程保护 |
| 同Tick callbacks / 轮次 | 各100000 | 前进性保护；0可在API中禁用 |

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
