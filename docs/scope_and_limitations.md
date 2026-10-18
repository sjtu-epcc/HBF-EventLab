# HBF EventLab 支持范围与限制

适用版本：0.5.0。HBF EventLab 实现 OCP HBF v0.7.0 的选定行为子集，提供有限workload的功能事件闭环。**它不是完整OCP实现，不声称通过一致性认证；时间参数尚未校准。** 规范关系与项目解释见[设计决策](design_decisions.md)，具体输入和退出码见[使用说明](usage.md)。

## 1. 已支持的能力

| 范围 | 当前能力 |
|---|---|
| Host与地址 | Global字节范围交织、跨条目补齐DLU、逻辑片段归属、有限发送与恢复保留区。 |
| 写路径 | 64B命令、显式完整4KiB local burst、组装位图、超时、overlap、接收/program顺序和起点erase。 |
| 读路径 | 64B读、无序Pending转发/缺片、普通有序组、Cache、Regular/Batch窗口、合法性重查。 |
| 资源 | 每Bank一个介质操作、独立Bank重叠、双页Cache、有限RX/TX、UCIe序列化/传播与理想Core路由。 |
| 恢复 | Host对0x4、0x5及关联0x6/info1的有界重试；program-fail replay、最远终点合并、旧尝试排空、预算/数据不足退出。 |
| 映射与容量 | 组件API提供zone交换、同Bank备用块替换、稀疏补偿及永久逻辑退役；并非所有能力都已成为CLI命令。 |
| 运行 | 确定性因果事件、定向唤醒、活动排空、停滞和时间/事件限额。 |
| 统计 | 统一生命周期接口，Global/原始逻辑工作/命令尝试分层，五个时刻、分段及端到端延迟；每channel Pending总数及阶段曲线、已收片段数、上下文起止与终结原因；错误/未完成记录与可选JSON。 |

## 2. 响应子集

读写状态独立定义，相同数值含义不同。以下“支持”仅指现有路径和故障输入，不表示完整异步故障覆盖。

| 读状态 | 当前处理 |
|---|---|
| 0x0 Success | Pending转发或普通Cache/介质读取成功。 |
| 0x1 InvalidAddress | 已建模越界或永久逻辑退役地址。 |
| 0x2 TemporarilyRestricted | 项目选择用于恢复未完成或物理目标待修复的普通读；Pending分支优先。 |
| 0x3 InvalidUserField | 已建模的非法read_hint等字段；不等于完整AXI user检查。 |
| 0x7 UnwrittenPage | 未写页；也用于remap后违反重写前禁读前提的防御性响应，后者是项目选择。 |
| 0xA PendingWriteMissing | 当前Pending DLU缺少目标64B片段，不能回退旧Cache/NAND。 |

| 写状态 | 当前处理 |
|---|---|
| 0x0 Success | Core program成功后形成父command响应。 |
| 0x1 InvalidAddress | 已建模非法或退役逻辑地址。 |
| 0x2 OverlapAddress | 当前组装重复、正在处理页及已写非起始页等覆盖规则。 |
| 0x3 InvalidUserField | 已建模字段检查分支。 |
| 0x4 PendingDluLimit | 新DLU名额耗尽；已有DLU的合法补片可继续。 |
| 0x5 AssemblyTimeout | 期限内未收齐4KiB，终结该DLU关联命令。 |
| 0x6 WriteOrderViolation | 当前写序违例；前页program失败影响的后继使用附加信息0x2。 |
| 0x7 ProgramFail | 失败页及其父命令触发Host恢复。 |
| 0x8 CapacityUnusable | 当前无可用替换目标的block处理；未覆盖完整Die或异步退役传播。 |

未实现读0x4/0x5/0x6/0x8/0x9、写0x9及其他未列状态的完整行为。CLI会提前拒绝部分无效workload，不能通过CLI测试所有低层协议错误；相关路径测试直接调用组件或公共事务接口。

MOCS满采用未接受背压，没有额外协议completion；Pending DLU满是已建模错误响应。`NotTaken`、路径 `Backpressured`、OCP状态、Host逻辑失败和Runner停止状态是不同层面的结果。

Host默认重试0x4失败命令，并在0x5排空后重发完整页；0x6/info1只修复能关联到前页资源错误的具体尝试，不是通用自动纠错。每类重试可独立关闭或限制次数；超时页缺少完整输入/保留数据、持续超载或根因无法关联时明确失败。参数、失败诊断及统计单位见[使用说明](usage.md#写重试与自动恢复)。

## 3. 明确不支持的输入组合

<a id="ordered-pending"></a>

| 限制 | 当前处理与影响 |
|---|---|
| `LIMIT-READ-001`：显式有序读访问Pending DLU | 成功转发和缺片0xA组合均在接受副作用前给出模型能力诊断；运行器报告ModelError。有限返回缓冲不自动解决任意顺序等待时的Pending快照问题。 |
| `LIMIT-WRITE-001`：额外WRITE完成依赖 | Host加载时拒绝；避免提供缺片的后序写反而等待前序DLU完成。规范本身并未因此禁止Host依赖。 |
| `LIMIT-CONTENT-001`：同块内容ID非连续回用 | 不支持A→B→A；新内容使用新的身份。 |
| `LIMIT-CONTENT-002`：同内容同64B写片段重复输入 | Host加载时拒绝；此限制不替代Device overlap规则。 |
| `BURST-INCOMPLETE` | 显式写burst必须完整覆盖同内容同local DLU，不补零，不自动生成缺失数据。 |
| 读burst、scratchpad、CSR/ADMIN | 不在当前输入子集内。运行配置静态冻结。 |

同组读按channel分别排序，不形成跨channel全局顺序。文件行顺序不隐含前一Global完成；READ可用`after`显式依赖。任意新旧内容读快照语义尚未实现。缺前页可导致Stalled；未覆盖整页的输入经timeout后会因MissingRetryData结束为逻辑失败。若输入包含未来补片，Host可等待其`at`边界后整页重试；这不补造workload中缺失的数据。

## 4. 介质、故障与维护边界

- 使用 `DataToken` / `PageData` 保存符号身份，不模拟真实用户字节、NAND cell电荷、ECC位或无碰撞完整性校验。
- 未实现CECC/UECC、读重试与刷新全流程、温度/热节流、完整Die故障门控、广域异步故障传播和完整erase-fail恢复。
- 恢复范围采用从block起点到失败页的既定解释；相关章节差异保留在设计决策中。Host从自己的版本化保留区生成replay请求，不从NAND私有状态补齐缺失数据。
- CLI没有自动refresh、zone-remap或wear-leveling workload；组件接口存在不等于端到端维护策略已经实现。
- 块替换限制在同channel/die/bank；没有页级FTL、CMT/GTD访问模型、SSD GC或任意跨lane搬移。稀疏补偿预算和匹配机制是项目研究选择，不保证任意zone pair都能立即交换。

## 5. 协议、性能与规模边界

当前按事务建模，写控制与数据关联一次提交；不实现逐拍AXI、AW/W独立乱序、完整AXI ID/user位编码、RRESP/BRESP映射或完整UCIe flit/credit协议。显式`read_group`只承载当前组序语义。

Bank全程互斥、组头取数、每遍排空和逐页replay是保守基线。Die共享端口/总线未完整建模，Core Channel无限带宽/零延迟；Cache替换当前硬编码，不能宣称所有资源与策略均已插件化。

固定NAND/Cache时延、控制服务字节、传播延迟、缓冲深度、演示几何和Host容量是模型参数。UCIe有效吞吐档位有规范来源，但不构成整条路径的硬件校准。Tick、命令计数、CTest墙钟耗时和C++容器内存分别属于不同指标，不能直接推导真实吞吐、功耗或面积。

Host加载有限输入并保留工作历史，部分检查扫描候选和依赖；尚未针对百万命令优化。可选统计collector保留请求、尝试及组装变化的精确历史，增加宿主内存与分析开销；不改变模拟时刻，不代表硬件缓冲容量。Pending曲线和各阶段占用已提供，但Programming数量不包含read/erase，不能替代Bank利用率。统一输出尚不含Bank占用曲线、Bank利用率和宿主墙钟耗时。没有GPU/HBM前端或完整GPU–HBM–HBF执行路径。Linux及其他编译器的验证状态以[验证说明](verification.md#evidence)为准。

## 配置入口的边界

统一配置支持静态几何、能力／选定值、Host、执行模型、故障输入和运行限额，完整字段见[参数说明](parameters.md)。支持的内建策略为Host `fcfs`（默认）／`fill_open`、读窗口 `window`、NAND `fcfs`、UCIe返回 `fifo`；没有实现的策略名称会报错，不能只靠改一个字符串获得新策略。配置在运行前冻结，不模拟CSR/ADMIN热修改。Host FCFS作用于每channel的当前合法候选，允许越过受阻请求；不提供严格队首阻塞模式，也不保证容量过载或不完整输入能成功。

UCIe档位派生规范给出的带宽和上限，实际几何仍可使用较小的功能配置。合法MOCS不保证前进：64B写模式下的明显credit不足只产生workload诊断，运行仍可能超时或失败；纯读及完整写burst不会因同一规则被提前拒绝。固定时延、缓冲深度和控制服务量均未经过产品校准。
