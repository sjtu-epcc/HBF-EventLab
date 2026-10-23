# HBF EventLab 架构

HBF EventLab 是面向 OCP HBF v0.7.0 选定行为子集的离散事件模拟器。本文描述当前实现及所有权边界；规范依据与项目解释见[设计决策](design_decisions.md)，输入和运行方式见[使用说明](usage.md)，能力边界见[支持范围与限制](scope_and_limitations.md)。

统一配置的装配顺序为：内建默认值 → INI文件 → 显式CLI覆盖 → `resolve_configuration()`校验与派生 → 冻结配置 → 构造运行组件。`SimulationConfiguration`按device、address、host、execution、faults、run、output分组；组件接收各自的类型化子配置，不读取配置文件，也不共享一个可修改的全局参数对象。

`StaticDeviceDescription`持有几何、能力、设备选定参数、Core拓扑及容量补偿预算。`ExecutionConfiguration`组合介质时延、UCIe档位与开销、缓冲及读调度配置；Host保留自己的发送、恢复及输入限制。统一入口负责跨组校验和策略装配，合法性过滤仍由运行组件执行。具体参数与来源见[参数说明](parameters.md)。

## 1. 自动运行主线

```text
workload 文件 → read_workload → GlobalRequest[]
    ↓
HostDriver：GlobalRequestSplitter + IHostInterleaver
    → 逻辑片段 / 显式完整 DLU 写事务
    → 按 channel 待发送集合 → IHostSendPolicy
    ↓ IHostPort::try_submit
DeviceDriver：请求链路 → RX → 正式接受
    ├─ WritePath → 组装 / erase / program 候选
    └─ ReadPath → Pending / 错误 / Cache / 读窗口候选
    ↓ Device 检查合法性和资源，INandSchedulingPolicy 选择可发射操作
Bank / Cache → 介质完成 → 有限 Egress → UCIe 响应仲裁与传输
    ↓ DeliveredResponse
HostDriver：结束尝试、汇总 Global 工作，或驱动恢复任务

EventQueue + SimulationRunner 推进事件并检查 Host、Device 活动
```

Host 只依赖公开的 `IHostPort`、静态地址视图和自身记录，不查询设备的 `ReplayState`、物理映射或 NAND 私有数据。文件解析位于适配器层，核心 Host 不依赖文本格式。

## 2. 接口与职责边界

| 组件 | 职责与主要入口 |
|---|---|
| `StaticDeviceDescription`、`ExecutionConfiguration` | 冻结几何、能力和运行参数；保持参数校验与运行状态分离。 |
| `IHostInterleaver`、`GlobalRequestSplitter` | Global 字节地址交织与范围拆分，保留逻辑片段的 Global 归属。 |
| `HostDriver` | 输入校验、合法候选、发送与反馈、内容版本、页级资源重试、Global 最终结果。 |
| `IHostSendPolicy` | 在 Host 已过滤的合法候选中选择发送优先级；内建 `fcfs`（默认）与 `fill_open`。 |
| `IHostPort` | 事务移交、异步正式接受通知、最终响应；读写响应类型分开。 |
| `DeviceDriver` | 传输、正式接受、候选管理、资源占用和完成事件的统一协调。 |
| `CommandAdmission` | 读写共用的每 channel command credit，正式接受时登记、交付时释放。 |
| `WritePath`、`WriteAssembly`、`BlockWriteOrder` | 固定写合法性、完整组装、接收/program 顺序、erase/program 生命周期。 |
| `ReadPath` | 接受侧观察、逻辑依赖、候选重查、物理绑定、结果及响应生命周期。 |
| `ReadDispatchWindow`、`IReadDispatchPolicy` | 每 channel 的读累积集合，以及 Batch、数量和 timer 释放规则。 |
| `INandSchedulingPolicy` | 从合法且资源允许的介质操作中选择优先级；内建默认 `fcfs` 和可选 `read_priority`，均不抢占、不改写同 Bank sense 顺序。 |
| `IUcieArbitrationPolicy` | 独立的传输选择接口；响应侧仲裁可传输结果，请求侧待发顺序由 Host 选择。 |
| `CapacityRetirementCoordinator` | Zone remap、备用块替换、补偿表和永久逻辑退役的一致提交入口。 |
| `RecoveryManager`、`WriteRetentionStore` | Host 恢复范围、每遍排空和预算；有限、版本化的恢复 token 保留。 |
| `EventQueue`、`AdmissionWaitRegistry` | 时间与因果顺序、取消和限额；按原因/对象的异步唤醒。 |
| `SimulationRunner` | 驱动事件并查询状态摘要，区分完成、停滞、限额和模型诊断。 |
| `IStatisticsSink`、`StatisticsCollector` | 接收带事件时刻的观察记录，关联Global、逻辑片段和命令尝试，提供只读统计及JSON；不参与调度。 |

overlap、写顺序、错误作用域、同组交付、同 Bank sense 顺序和所有权是固定正确性条件。策略可以改变合法候选的优先级，不能绕过这些条件。接口存在也不意味着所有替代实现均已通过验证。Core Channel 目前只有路由模型，Cache 替换也尚无独立策略插件。

Host 将稳定发送单元身份 `SendUnitId` 与本次入队序号分开维护。FCFS 按每 channel 合法候选的入队序号选择，允许合法补片越过受阻请求；`fill_open` 先比较补齐／replay 优先级，再比较同一序号。初始入队依释放时间和输入次序登记，错误重试重新排队，`NotTaken` 保留队龄。入队序号只影响 Host 选择，不改变 Device 的接收、介质调度或 UCIe 仲裁规则。

读统计复用真实窗口及Device状态转换：`ReadDispatchWindow`在释放时报告成员和触发原因，Device关联为稳定attempt身份；Cache pin和成功sense提交分别记录。统计接收器不管理窗口，也不产生事件。读优先只改变已就绪合法候选的选择，窗口尚未释放的读不能因优先级高而提前发射。

## 3. 地址与映射

```text
GlobalByteAddress
    ↓ IHostInterleaver：减一次 global base，并交织
ChannelAddress = channel + LocalByteAddress
    ├─ logical_view → LogicalNandAddress（逻辑 block / page / offset）
    └─ ZoneMapping → MediaAddress
                         ↓ AddressLayout::decode
                     NandSlotAddress（nominal 槽位）
                         ↓ RetiredHoleMap 命中，或 BlockReplacement
                     PhysicalNandAddress（实际目标）
```

`LocalByteAddress` 是 channel Flash 逻辑空间从零开始的字节偏移；图示的64B编号由独立类型 `LocalAddress64B` 表达。Host 顺序与恢复使用逻辑地址。备用块是额外物理容量，不增加 Host 可见地址范围。

ZoneMapping 交换 zone 的目标，不搬移用户数据；Host 必须声明涉及数据可丢弃，并完成必要排空。替换表跟随介质槽位，在 zone 映射之后生效，当前保持 channel/die/bank，只改变 block。RetiredHoleMap 保存最终目标，不形成映射链。永久逻辑退役不会因交换复活。

`inspect()` 仅供观察，实际访问用 lease 固定映射。未发射读先持逻辑内容依赖；绑定物理地址与 lease 后移到实际执行边界。

## 4. 事务边界与有限资源

| 边界 | 所有权与可观察结果 |
|---|---|
| `NotTaken` | Host 保留发送单元，订阅等待条件；没有 completion。 |
| `Taken` | Device 登记唯一移交身份、预留 RX 并开始序列化；Host 从此持续跟踪未结尝试。 |
| 到达 RX | 预留变为驻留，尚不表示占用 MOCS；资源不足时留在 RX 等待。 |
| 正式接受 | 路径接受并占用 command credit，释放 RX；异步 `AcceptedNotice` 更新 Host 接受进度。 |
| 实际发射 | 复核路径条件与物理资源，取得操作 ticket 并安排完成事件。 |
| 结果确定 | 保存结果或固定错误；物理保护与响应存储按各自生命周期释放。 |
| 响应交付 | 恰好一次释放 credit/返回槽、推进组序，再以异步反馈通知 Host。 |

Host 一次提交写 metadata 和关联 payload，二者分别持有，不拆成两条 Host 命令；这属于事务级模型，不模拟逐拍 AXI AW/W 握手。

每个物理 Bank 同时最多执行一项 read/program/erase；独立 Bank 可以重叠。没有覆盖整个 Die 的全程互斥锁，也没有完整的 Die 共享端口模型。Core Channel 保留拓扑，按无限带宽、零延迟处理。

每 channel 的请求/返回方向分别计序列化服务；服务时间与固定传播延迟分开。RX/TX 都受条目和 payload 容量限制。序列化结束可服务下一事务，在途事务的预留存储仍保持到相应边界。

## 5. 写入生命周期

```text
WritePath::submit
    → credit、字段、逻辑地址与退役检查
    → 整条 command 的覆盖/顺序预检查
    → WriteAssembly + IAssemblyStorage
         ├─ 新 page-0：等待旧依赖排空，建立 erase 准备
         └─ 完整 DLU：形成 program 候选
    → probe_erase / probe_program（查询不推进业务状态）
    → Device 取得 Bank，issue_erase / issue_program
    → finish_erase / finish_program
    → 更新介质和顺序，形成各父 command 的响应
    → response_delivered
```

received bitmap 判断片段是否收到；完整4KiB才可 program。接收游标与 program 游标分开：前页全部正式接受后可接收后页，后页 program 还须等待前页成功。完整写 burst 拆成64个片段，仍只有一个父 command、一个 credit 和一次响应。

覆盖写起点等待旧组装、操作、响应、逻辑读和物理访问排空；未接受的新起点不创建 Pending DLU。组装超时可重试同页；page-0 超时重试复用仍有效的 erase 准备，不自动扩大为整块 replay。

`ReplayState` 保存设备尚未完成的恢复义务。故障诊断区分可重用失败与物理不可用，不从 Host 的0x7直接推导“块永久损坏”。同 Bank spare 替换和永久退役由容量协调器维护。

## 6. 读取、窗口与结果所有权

```text
ReadPath::submit
    → credit、字段/地址、退役、Pending 与恢复状态
    ├─ Pending已有片段：预留结果空间并固定转发值
    ├─ Pending缺片/已知错误：固定错误响应
    └─ 普通读：登记逻辑 guard、可选组序和候选
         ├─ 当前合法 Cache hit → 安排Cache读取
         └─ 需要sense → 进入每channel窗口
              → Batch / 阈值 / 主动timer释放调度资格
              → 重查Pending、限制、组序、映射、Cache和资源
              → 实际issue与完成
    → Cache结果移交有限Egress → 响应传输 → 交付
```

Pending 可见性先于 Cache；缺失片段不能从旧 NAND 或旧 Cache 补出。空间不足而未接受的读可以重新观察，已经固定的数据或0xA不会被后来补片改写。有序 Pending 组合当前报告能力诊断，详见[限制](scope_and_limitations.md#ordered-pending)。

窗口释放资格，不形成整批完成屏障。接受有效 Batch 提示立即关闭当前集合；即使该 B 被 Pending 转发或返回0xA，也触发边界。未接受请求没有此副作用。尾 Regular 有主动 timer，不依赖后续请求到达。

普通同组读采用组头取数的保守规则；组序按 `(channel, group)` 管理，错误交付也推进组头。同组请求可以访问不同页。设备另检查同 Bank sense 顺序，高优先级受阻不应阻止其他合法 Bank 推进。

每 Bank 两个4KiB Cache槽。sense 前必须有目的槽；未保护槽中优先使用无效槽，再按 LRU 选择。介质完成释放 Bank，但读结果通过 Cache 引用保持到有限 Egress 可接手；TX 拥塞可最终阻止同 Bank 的新 miss。失效标签不能破坏已经确定的受保护结果。

`IReadResultStorage`/`ReadResultLease` 表示有界数据所有权，Parent 只持句柄。普通读在 issue 采样符号内容、finish 固定结果，随后释放逻辑 guard 和物理 lease；响应仍可等待链路。Pending 转发不需要介质 lease。

## 7. 状态所有权

| 所有者 | 关键状态 | 更新或释放边界 |
|---|---|---|
| 静态描述 | geometry、capability、选定参数、Core拓扑、容量预算 | 单次模拟冻结。 |
| 容量协调器及映射组件 | zone映射、替换、RetiredHoleMap、逻辑退役、内容代次、lease | 排空、失效与一致提交。 |
| `NandMediaState` | 物理页符号内容、不可用状态、擦除历史 | 介质操作完成。 |
| `CommandAdmission` / `ReadOrderTracker` | command账本与组内命令队列 | 接受／响应交付。 |
| `WritePath` / `WriteAssembly` / `IAssemblyStorage` | 准备状态、ticket、父响应、位图、deadline、PageData | 组装、操作终止及交付。 |
| `BlockWriteOrder` / `ReplayState` | 接收/program游标、活动页、恢复终点及绑定 | 收齐、program结果、合法重启或显式内容废弃。 |
| `ReadPath` / `LogicalAccessTracker` | 候选、逻辑依赖、操作、固定结果句柄 | 结果稳定/错误终结释放依赖；交付释放结果。 |
| `DeviceRuntimeState` / `BankCache` / `EgressBuffer` | Bank ticket、Cache槽及引用、有限返回存储 | 操作完成、结果交接和响应交付。 |
| `DeviceDriver` | 移交事务、RX/TX、窗口、候选索引、拥有的事件 | Taken至最终反馈。 |
| `HostDriver` | Global与片段结果、发送单元、尝试表、暂定接受bitmap、内容版本、`PageRetry`、容量反馈版本与重试timer | 响应修正发送进度；页修复、重发和逻辑最终归属；不复制设备Assembly或Pending账本。 |
| `RecoveryManager` / `WriteRetentionStore` | 恢复任务、旧尝试集合、终点、预算、固定token前缀 | 每遍排空、重发、成功或有限失败。 |
| `EventQueue` / `AdmissionWaitRegistry` | 时间、因果轮次、事件/订阅ID、通知版本 | 调度、回调、取消。 |

context generation 和操作ID防止旧事件作用于新对象，不是 block 重写epoch，也不是 PEC。显式 discard/remap 的内容代次只标识被放弃的逻辑内容；普通page-0或spare替换不借此清除未完成恢复。

## 8. Host反馈恢复

页级资源修复由 `HostDriver` 管理：`Version::accepted` 是暂定发送进度，`Version::retries` 记录当前页的 `Repairing`、`Draining` 或 `AwaitingPredecessor` 状态，`Unit` 保存该发送单元的重试原因和次数。正式接受错误命令也会置位，因此收到错误后必须修正，不能把该bitmap当作Device的数据驻留证明。

0x4仅重试失败命令，保留有效补片；0x5等待该页旧移交尝试全部终结，核验完整数据后整页重发。后继0x6/info1与前页资源错误关联，修复按最早受影响页推进。容量相关完成反馈或正时间间隔timer触发重查；无效timer会取消，活动修复状态纳入Host排空摘要。具体边界见[页级重试决策](design_decisions.md#adr-loop-h20)。

这些页级状态与 `RecoveryManager` 的program-fail任务分开；replay内的资源重试不创建新的block恢复任务。下面的block恢复屏障只用于program-fail流程，不能直接套用于仍有有效Pending片段的0x4修复。

Host从自己的命令表定位0x7失败页。先收到后继0x6/info2时等待真正根因，不把后继地址当成失败位置。每遍失败冻结同块已经移交而未处理最终反馈的有限旧集合，包含Link、RX和已接受的读写；新到普通读不持续扩大该集合。

旧集合排空且根因已知后，Host核验并固定原内容的恢复前缀，从保存的Global地址和token重新生成普通写。多响应合并一个任务；再次失败保留最远未完成终点，预算不重置。默认最多三遍，每遍首条page-0正式接受时计一次；逐页等全部成功响应后推进。

数据不足或预算耗尽结束相关逻辑工作，继续排空已移交事务，其他块可继续。不会读取NAND私有状态补数据、清空设备恢复义务或虚构容量退役。具体解释见[恢复决策](design_decisions.md#adr-loop-h11)。

## 9. 事件、通知与停止

正常事件按 `(Tick, round, phase, EventId)` 确定顺序。同刻回到相同或较早phase进入下一轮，较晚phase可在当前轮执行。所有正常轮次排空后，冻结同Tick组装deadline集合并整批结算，再处理超时反馈；窗口timer属于正常事件。

`run_next()` 可以执行整个deadline批次，`executed`按真实callback计数；限额先检查整批预算。事件/轮次限额保留现场，不增加虚构Tick或强制完成请求。回调异常消费本事件，剩余冻结批次保持优先。

等待通知按原因与对象订阅；revision复核避免漏唤醒，多原因是OR，通知只表示条件可能改变，回调必须重新检查。重复通知合并，取消包含已排队通知。物理lease析构只更新计数和dirty记录，在稳定边界调用 `publish_releases()`，避免析构重入。

Runner只查询各所有者摘要。有未来timeout、窗口或完成事件就直接跳时；无有效事件而仍有工作才报告Stalled。Completed需要Host与Device瞬态都排空，逻辑成功另报。持久NAND、映射、PEC、无活动引用的Cache和已放弃恢复留下的限制无需清零。

## 10. 集成与生命周期要求

正式入口见[CLI源码](../examples/minimal_loop.cpp)。构造关系为：静态描述与配置 → EventQueue → DeviceDriver → interleaver → HostDriver → SimulationRunner。端口、地址转换器和时钟必须比Host存活更久；外部EventQueue必须比Device存活更久。

低层组件测试可以显式构造路径与有界结果替身；正式闭环由Device安排完成，Host不得直接调用finish或response_delivered来代替反馈。回调异步执行，析构仅用于关闭模拟实例，不是运行时撤回已开始NAND操作的协议。

容量协调器提交前检查排空、数据可丢弃、健康资源与密度，准备候选后同步失效，再一致提交。失效回调不可重入修改或获取新pin；异常保留旧映射与quiesce，已经发生的物理损坏不回滚。附着的映射组件不能绕过协调器修改。

## 11. 统计观察边界

Host与Device通过`IStatisticsSink`发布已经发生的生命周期事实；`StatisticsCollector`只保存观察历史及汇总，不参与发送、组装或资源调度。请求统计与Pending统计共用一个sink；默认不挂接，开启后也不添加采样事件或模拟Tick。

`WriteAssembly`仍唯一拥有Pending上下文、received bitmap和阶段。其`accept`、`queue_program`、`start_program`、`retire`在状态提交后发布只包含值的`AssemblyObservation`；`WritePath::set_statistics_sink`补上`EventQueue::now()`，`DeviceDriver`负责装配。program提交失败抛出异常时，不发布虚假的retired。析构仅释放宿主资源，不属于模拟终结事件。

统计按`(channel, local_dlu, generation)`保存上下文历史；按channel聚合Collecting、Ready/ProgramQueued、Programming及总数，输出变化点曲线和占用积分。观测记录内的命令成员只包含真正加入Assembly的命令；通过传输尝试连接Host逻辑工作。统计历史是模拟器的分析存储，不增加被模拟的组装容量。完整字段、终结原因、活动记录和曲线重建规则见[Pending统计说明](usage.md#pending曲线与dlu上下文)。

Bank状态仍由`DeviceRuntimeState`唯一维护。`DeviceDriver::execute_media`在成功发射与实际释放Bank后发布`StatBankOperationStarted/Finished`，复用执行ticket和事件时刻；`StatisticsCollector`保存`bank_operations`历史，供离线积分。准备失败／回滚不记发射，析构不伪造完成。统计仅保存观察事实，不能作为Scheduler判断Bank可用性的依据。容量实验工具直接消费这些记录和原有Pending／命令历史，相关口径见[实验说明](../experiments/capacity/README.md)。
