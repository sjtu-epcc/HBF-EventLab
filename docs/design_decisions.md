# HBF EventLab 设计决策与 OCP 规范追溯

适用项目版本：**0.5.0**。

本文记录 HBF EventLab 的 155 条设计决策，包括规范关系、条目性质、选择理由、实现位置、验证等级及已知限制。决策以 ADR 和 ADR-LOOP 标识，并通过关联要求和测试追溯其适用范围。

规范基准为 [OCP HBF Architecture Specification v0.7.0（03 AUG 2026）](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf)，主要对照第 4、5、11 章。规范关系和项目选择必须分开记录；实现一个接口不等于证明任意替代实现都符合规范。

## 1. 阅读方法与证据边界

每条决策使用 **规范关系** 和 **条目性质** 两个维度。分类对象是具体规则、机制或假设；“分类边界说明”用于区分一条记录中不同内容的依据。

### 1.1 维度一：规范关系

该维度说明规范对本条内容确定到什么程度；不是符合性评分。

| 关系 | 判断依据 |
|---|---|
| 直接规定 | 规范明确给出适用条件下的行为、公式、限制或外部结果。实现该规则的代码可以直接关联本条，不因采用C++类就变成另一类。 |
| 直接推导 | 从明确条款/公式即可得到，不额外依赖项目选择。例如4KiB/64B=64及映射保留条带内偏移。 |
| 受规范约束，机制未指定 | 规范限定必须满足的行为，但没有唯一决定本条的机制、表示或策略。必须指出关联要求；不意味着机制已经被证明符合规范。 |
| 规范歧义或未明确边界下的项目解释 | 条文存在解释分歧或必要的行为边界未完整定义，项目选定一种解释。保留相关原文位置、理由和影响；一般的实现自由不自动属于歧义。 |
| 无直接规定 | 模型输入/初态/范围假设、宿主程序保障或没有直接数值依据的参数等。可能间接服务规范要求，但不应虚构直接条款。 |

“受选定解释约束”“增加模型限制”“对应仍待确认”等为上述关系的必要限定，不另建互斥类别。依赖项目假设的推导不标为不附条件的“规范直接推导”。

#### 1.1.1 “受规范约束”与“无直接规定”的操作判据

先确定条目真正描述的对象：是外部行为、该行为的实现机制，还是额外模型前提、数值选择或宿主程序保障。不能按“规范有没有出现类名/算法名”分类，也不能仅凭“它有助于实现规范”就归为受规范约束。

| 判别步骤 | 受规范约束，机制未指定 | 无直接规定 |
|---|---|---|
| 明确直接约束 | 能定位条款和适用条件，并写出它要求保留的行为、结果或不变量。 | 本条直接描述的模型输入、初态、附加范围、数值或宿主保障没有对应的直接约束；仍可列间接关联。 |
| 建立对应关系 | 能说明本机制的哪些状态、步骤或边界落实上述约束；替换机制后，同一约束仍必须成立。 | 与规范的联系停留在支持运行或额外研究前提；不能从相关条款得到本条所述选择本身。 |
| 说明自由度 | 规范决定应满足什么，但允许多种机制/表示/策略；记录本项目选用哪一种。 | 说明选择来自模型范围、演示输入、平台要求或缺乏校准的默认值，不补造规范理由。 |
| 区分证据不足 | 有明确规范约束，但尚未证明该实现满足它，仍可归此类，同时如实记录未验证或待确认。 | 未查到条款不等于规范没有规定。尚未完成核对时应写“依据待确认”，不能仅因证据缺失就断言无直接规定。 |

建议在“分类边界说明”中依次写：**本条对象 → 条款及适用条件 → 约束如何作用到该对象 → 项目自行选择的部分**。无法完成第三步时，应进一步拆清描述对象，或保留依据待确认，不以模糊的“保证正确性”代替对应关系。

还需遵守以下边界：

1. 记录的若就是规范给出的错误码、禁止行为或公式，应标“直接规定”；用代码实现它不使其自动变成“受规范约束”。从明确规则无额外项目前提即可算出的结果属于“直接推导”。
2. received bitmap 用于落实不得重复覆盖及收齐后下发的规则，属于受约束的表示机制；位宽计算若仅为 4096/64，则是直接推导。64 位整数溢出诊断作为宿主数值保障，没有因此自动成为规范条款。
3. “稀疏映射预算”可以服务既有映射机制，但“默认 50‰”的数值若无条款依据或校准，仍属无直接规定。不能用整体机制的关联要求替该数值背书；默认数值的依据仍须独立说明。
4. 外部可见的映射/退役一致性与 C++ 候选快照、异常安全、对象身份检查应分别说明：前者需给出具体规范约束，后者若仅为宿主保障可为无直接规定；若条目直接描述保障外部一致性的提交机制，则应把对应约束写清。混合条目须分别说明这些对象的依据。
5. 依赖已选项目解释的机制应写“受选定解释约束”并链接该解释；依赖项目模型前提的结论不能写成无条件的规范直接推导。
6. 规范明确留给产品选择的原因、时间或策略，通常是受规范约束的选择空间，不因它不唯一就成为“规范歧义”。只有需要解释的行为边界确实不清楚或条文冲突时，才使用项目解释类别，并指出具体边界。
7. 同一条目包含多个对象时可保留原 ID，但分类标签必须逐项对应；“机制未指定”不等于“无直接规定”，“无直接规定”也不等于没有合理工程必要性。

### 1.2 维度二：条目性质

该维度说明条目记录的是什么，不表示它是否已实现或已验证。

| 性质 | 含义 |
|---|---|
| 行为要求 | 需要遵守的行为、条件或结果；若来自项目解释，显式标“行为要求（项目解释）”。 |
| 实现机制 | 采用的数据表示、状态组织、处理流程或组件协作。普通类命名和函数拆分不因未写入规范就自动构成独立设计决策。 |
| 模型／输入假设 | 模拟范围、输入边界、初始化状态、抽象层次、参数用途及额外配置限制。 |
| 可配置策略 | 明确提供参数选择/策略入口的决策；仅预留接口时注明“接口预留”。硬编码但可能有替代方案的算法仍记录为实现机制。 |
| 实现保障 | 数值、身份、生命周期、事务与不变量检查等，用于维持当前实现的正确性。 |

一个条目可以包含多种性质，但须明确各标签对应的内容。例如，地址公式、输入适配和类型保障具有不同依据，应在分类边界说明中分别解释。

### 1.3 字段与验证等级

“选择理由”说明技术取舍；“模型假设/参数依据”区分规范规定、模型前提和未校准的数值。尚未确定的依据须如实标明。“实现状态”与“验证状态”分别描述功能是否存在和证据覆盖到什么程度。

| 验证等级 | 含义 |
|---|---|
| V1 | 有针对性断言，关联用例在所列验证记录中通过。 |
| V2 | 仅覆盖部分条件、独立组件或部分集成边界。 |
| V0 | 主要为静态确认、接口或参数说明，缺少专项测试；标为参数校准 V0 时表示没有物理校准证据。 |

验证等级不表示完整 OCP 符合性或认证。可配置接口存在，也不代表所有替代策略均已验证。

### 1.4 规范与实现证据

规范条款以 OCP HBF Architecture Specification v0.7.0 为基准；实现行为以源码、测试断言及相应验证记录为依据。代码链接使用仓库相对路径，行号用于定位。验证环境、日期和结果统一记录在验证文档中。

| 证据 | 用途与限制 |
|---|---|
| [OCP HBF v0.7.0 官方页面](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf) | 规范条款、表格和图示的来源；本文主要对照第 4、5、11 章。 |
| [架构说明](architecture.md)、[使用说明](usage.md) | 当前组件职责、输入范围和运行约定。 |
| [验证说明](verification.md#evidence) | 测试环境、结果、覆盖范围及复现命令。 |
| `include/`、`src/`、`tests/`、`examples/` 与 `CMakeLists.txt` | 实现位置、断言和测试注册。 |

## 2. 关联要求索引

这些 REQ 是本表自定义追溯键，不是OCP官方编号。

| 键 | 要求/约束 | 依据 |
|---|---|---|
| REQ-ADDR | 64B/4KiB、地址单位、channel隔离、布局/可访问容量 | §4.1 p15、§4.3 p17、§4.6 p22–23、§5.7 p63 |
| REQ-ASSEMBLY | 4KiB完整后下发Core | §5.4.1 p58 |
| REQ-MOCS | command outstanding能力及选择 | §5.9.2.1 p70、§5.9.2.3 p71–72 |
| REQ-PENDING | 每channel Pending DLU上限及允许补片 | §5.4.1(7)、Table13、§11.5.2.4 |
| REQ-OVERLAP | 拒绝重复覆盖，包含pending和已写NAND | Table13 p61、§11.5.2.2 p121 |
| REQ-WRITEORDER | 同一block顺序、不同block可独立、错误范围 | §5.4.1、Table13、§11.5.2.6 |
| REQ-RESTART | 起点重写、auto-erase | §5.4.1(5)、§11.5.2.2 |
| REQ-TIMEOUT | 首片起算，缺数据时DLU范围错误 | §5.4.1(6)、Table13、§11.5.2.5 |
| REQ-RESPONSE | non-posted、每command一次响应、错误范围 | §5.4.1(1)(2)、Table12/13 |
| REQ-REPLAY | program失败传播与Host恢复 | §5.7、§11.5.2.6–7；终点有章节差异 |
| REQ-ZONE | 等大zone、swap、Host保证数据无效、设备不搬移活数据 | §5.2.4.6、§11.4–11.4.1 |
| REQ-CAPACITY | 不可恢复容量及后续访问错误 | Table12/13、§11.5.1.8、§11.5.2.1/8 |
| REQ-READFORWARD | 读pending写的已收片段转发，缺片段错误 | §5.4.3 p59 |
| REQ-READDISPATCH | Regular/Batch及bank/cache约束 | §5.3.1–2 p56–57 |
| REQ-MODEL | 模型抽象、参数证据、数值与所有权正确性 | 项目自定要求，无统一OCP条款 |
| REQ-SCOPE | 明确已实现子集，不将缺失行为称为符合规范 | 项目发布与可复现性要求 |

## 3. 统一决策索引

索引列出全部 155 条决策。详细记录说明每项规则的适用条件、实现机制和验证边界；关联条目用于解释同一行为在路径、资源和 Host 层的协作。

| ID | 内容 | 规范关系 | 条目性质 | 实现与验证状态 |
|---|---|---|---|---|
| [ADR-A01](#adr-a01) | Host 粒度为 64B，Core 写入及 DLU 为 4KiB；一 DLU 含 64 个片段。 | 直接规定；片段数为直接推导 | 行为要求 | 已实现；V1 |
| [ADR-A02](#adr-a02) | 内部 LocalByteAddress 是通道 Flash 区域零起点字节偏移；64B 编号另设类型并显式换算。 | 内部字节偏移：受规范约束，表示未指定；强类型：无直接规定 | 模型／输入假设；实现保障 | 已实现；V1 |
| [ADR-A03](#adr-a03) | Host 地址按条带轮转到 channel，并移除 global base；条带内偏移不变。 | 映射公式：直接规定；条带内偏移保持：直接推导；global base适配：无直接规定 | 行为要求；模型／输入假设 | 已实现；V2 |
| [ADR-A04](#adr-a04) | 按 bank→die→page→block 解码 DLU；encode_logical 是其逻辑视图的逆变换。 | 图示bank/die条带：直接规定；完整扁平布局与逆变换：受规范约束，机制未指定 | 行为要求；实现机制；模型／输入假设 | 已实现；V2 |
| [ADR-A05](#adr-a05) | 逻辑块、nominal slot、最终 physical block 分别建模；顺序状态使用逻辑块键。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-A06](#adr-a06) | Geometry、Capabilities、DeviceOperatingConfiguration 在 StaticDeviceDescription 构造后冻结。 | 无直接规定；属于当前模拟范围选择 | 模型／输入假设 | 已实现静态描述；V0 |
| [ADR-A07](#adr-a07) | 容量、时间及身份编号的关键加乘操作检查 uint64 溢出。 | 无直接规定 | 实现保障 | 已实现；V0 |
| [ADR-A08](#adr-a08) | 几何检查channel上限16、die数为1/2/4/8/16；其余维度允许小型测试值。 | channel/die边界：直接规定；其余小型几何：无直接规定 | 行为要求；模型／输入假设 | 已实现部分profile校验；V0 |
| [ADR-A09](#adr-a09) | spare为额外物理块，Host容量只计算nominal blocks。 | 受规范约束，机制未指定 | 实现机制；模型／输入假设 | 已实现；V2 |
| [ADR-A10](#adr-a10) | 块替换层统一使用 BlockReplacement；无替换记录时解析到默认物理块。 | 无直接规定 | 实现机制 | 已接入主线；V1 |
| [ADR-H01](#adr-h01) | Flash command 先按长度、对齐和页边界检查，再拆为保留父 ID 的 64B fragments。 | 长度/对齐/边界：直接规定；拆片和父ID保留：受规范约束，机制未指定 | 行为要求；实现机制 | 部分实现；V2 |
| [ADR-H02](#adr-h02) | 一个写 command 不论拆成多少片段只返回一次响应；成功需等 Core 数据提交。 | 直接规定 | 行为要求 | 已实现于写路径；V1 |
| [ADR-H03](#adr-h03) | MOCS 合法选择为 1 或 256..16384 的 2 次幂，selection 不超过 capability。 | 直接规定 | 行为要求 | 已实现；V2 |
| [ADR-H04](#adr-h04) | MOCS 满时不接受请求、不产生 completion、不创建介质工作；可选严格诊断模式。 | MOCS满后的处理：规范未明确边界下的项目解释；严格诊断：无直接规定 | 行为要求（项目解释）；可配置策略 | 已实现；V1 |
| [ADR-H05](#adr-h05) | command credit 按 channel 与父 command 计数，并保留到响应交付。 | command计数单位：直接规定；账本和抽象交付边界：受规范约束，机制未指定 | 行为要求；实现机制；实现保障 | 读写共享账本已接入；V1 |
| [ADR-H06](#adr-h06) | Pending DLU 满只拒绝打开新 DLU，已有 DLU 的未收片段仍可接收。 | 直接规定 | 行为要求 | 已实现；V1 |
| [ADR-H07](#adr-h07) | MOCS先校验Supported／Selected合法性；64B写前进性按实际workload提示，不禁止超载实验。 | 数值范围有规范依据；前进性诊断无直接规定 | 行为要求；模型／输入假设；实现保障 | 已实现；V1 |
| [ADR-H08](#adr-h08) | 读写路径共用按 channel 计数的父 command 账本，credit 保留到各自响应交付。 | 计数与响应约束：直接规定；共享账本：受规范约束，机制未指定；对象身份与禁复制检查：无直接规定 | 行为要求；实现机制；实现保障 | 已接入读写路径；V1 |
| [ADR-H09](#adr-h09) | 路径内先检查 MOCS，再处理已建模的命令错误与状态；公开事务入口先处理能力诊断；多错误共现使用固定优先级。 | MOCS 拒收及多错误共现裁决：规范未明确边界下的项目解释；检查组织：受选定解释约束，机制未指定 | 行为要求（项目解释）；实现机制 | 已实现当前子集；V2（未涵盖全部规范错误） |
| [ADR-H10](#adr-h10) | 地址越界与已建模 User 字段错误转换为 0x1/0x3；不支持的模拟器输入保留 API 诊断。 | 0x1/0x3 含义：直接规定；inspect_command 组织：受规范约束，机制未指定；支持范围及 API 异常边界：无直接规定 | 行为要求；实现机制；模型／输入假设；实现保障 | 已实现明确的输入子集；V2 |
| [ADR-W01](#adr-w01) | 组装 context 以 64 位 received bitmap 判断重复与完整性。 | 受规范约束，机制未指定 | 实现机制 | 已实现；V1 |
| [ADR-W02](#adr-w02) | 整条 command 在修改任何片段前检查 overlap，拒绝时不部分提交。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-W03](#adr-w03) | Assembly storage 与 context 元数据分离，正常路径保留整页数据到 program 终止。 | 受规范约束，机制未指定 | 实现机制；模型／输入假设 | 已实现；V1 |
| [ADR-W04](#adr-w04) | 重复片段、Ready/Programming 页及已成功写入的非起始页返回 overlap，原请求保持不变。 | 直接规定；状态覆盖含直接推导 | 行为要求 | 已实现于当前写路径；V1 |
| [ADR-W05](#adr-w05) | 同一 block 顺序接收，不允许跳页；不同逻辑 block 各自维护状态。 | 块内顺序/块间独立：直接规定，适用范围需结合上下文；分块状态：机制未指定 | 行为要求；实现机制 | 已实现于功能模型；V2 |
| [ADR-W06](#adr-w06) | 前一页全部接收后才可开始下一页；program 则等待前一页 program 成功。 | 接收边界：规范未明确边界下的项目解释；program依赖：受规范约束 | 行为要求（项目解释）；实现机制 | 已实现；V1 |
| [ADR-W07](#adr-w07) | 合法 page0 第一个片段安排一次 auto-erase；erase 完成前不 program。 | 起点auto-erase：直接规定；首个被接受片段触发及准备状态：受规范约束，机制未指定 | 行为要求；实现机制 | 已实现于扁平 page 模型；V2 |
| [ADR-W08](#adr-w08) | 新 page0 等待旧写、erase、响应、逻辑读依赖与实际物理 lease 排空；不要求旧 block 写满。 | 规范未明确边界下的项目解释 | 行为要求（项目解释）；实现保障 | 已实现；V1 |
| [ADR-W09](#adr-w09) | 组装 timeout 重试同一页，不自动扩大为 block replay；page0 复用已有 erase 准备。 | 允许整DLU重发：直接规定；page0复用erase：规范未明确边界下的项目解释 | 行为要求；行为要求（项目解释）；实现机制 | 已实现；V1 |
| [ADR-W10](#adr-w10) | Pending write 已收目标片段可转发，未收目标片段不能回退到旧 NAND。 | 直接规定 | 行为要求 | 无序 64B Pending 读响应已接入；V2（有序组合未支持） |
| [ADR-W11](#adr-w11) | 入口拒绝只完成当前 command；已存在 DLU 的 program failure 按关联集合结束请求。 | DLU 故障响应范围：直接规定；未形成 DLU 时的拒绝边界：规范未明确边界下的项目解释；reject 组织：受选定边界约束，机制未指定 | 行为要求；行为要求（项目解释）；实现机制 | 入口拒绝与现有 program-failure 广播已实现；V1；异步容量退役未实现 |
| [ADR-R01](#adr-r01) | Program fail 终止失败 DLU，并让同块已接受后续 DLU 返回 0x6/info2。 | 直接规定 | 行为要求 | 已实现于当前写路径；V1 |
| [ADR-R02](#adr-r02) | 故障恢复终点采用失败页；重放从 block 起点开始，成功到达终点后放行新页。 | 起点重放：直接规定；恢复终点：章节差异下的项目解释 | 行为要求；行为要求（项目解释） | 已实现；V1 |
| [ADR-R03](#adr-r03) | 重复 program failure 以 max(旧未完成终点, 新失败页) 合并一份恢复义务。 | 规范未明确边界下的项目解释 | 行为要求（项目解释）；实现机制 | 已实现；V1 |
| [ADR-R04](#adr-r04) | Timeout、再次 page0、换 spare 不解除恢复义务；通过恢复完成义务须顺序 program 成功至合并终点。 | 受选定恢复解释约束，机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-R05](#adr-r05) | 失败响应逐次结束旧请求；Host 用普通逻辑 Write 重发数据，不引入 Replay opcode 或自动复制。 | 直接规定；不复活旧请求为关联响应语义的直接推导 | 行为要求 | 设备语义及自动 Host replay 已接入；V2（有限 workload 子集） |
| [ADR-R06](#adr-r06) | 故障输入区分 FailedReusable 与 FailedUnusable，不能从 Host-visible 0x7 推导永久损坏。 | 两种错误语义有直接依据；故障输入划分受规范约束但机制未指定 | 模型／输入假设；实现机制 | 已实现为外部输入；V1 |
| [ADR-R07](#adr-r07) | 健康目标的新内容/refresh 重写只擦除并重写原物理块，不自动触发 remap。 | 受规范约束，机制未指定 | 实现机制；模型／输入假设 | 已实现；V1 |
| [ADR-R08](#adr-r08) | 显式 discard + remap 成功后可放弃旧内容及 replay 义务，永久逻辑退役不可撤销。 | 旧数据失效/逻辑退役约束有直接依据；清除replay义务为项目解释 | 行为要求；行为要求（项目解释）；实现机制 | 已实现；V1 |
| [ADR-Z01](#adr-z01) | 每个channel划分等大zone，当前配置要求zone恰好覆盖整个channel。 | 等大zone：直接规定；无尾部残区地覆盖channel：模型选择 | 行为要求；模型／输入假设 | 已实现；V2 |
| [ADR-Z02](#adr-z02) | zone size 额外要求为完整 block group 整数倍，保持 remap 后 die/bank/page。 | 受规范约束，但增加了模型限制 | 模型／输入假设；实现保障 | 已实现；V1 |
| [ADR-Z03](#adr-z03) | zone swap 交换当前目标，不复制、擦除或搬移用户数据。 | 直接规定 | 行为要求 | 基础 swap 已实现；带补偿语义见 ADR-C06；V2 |
| [ADR-Z04](#adr-z04) | Host 显式声明数据可丢弃；交换要求两 zone 均满足该声明，新 pin 消耗声明。 | Host保证数据无效：直接规定；声明API和消费标志：受规范约束，机制未指定 | 行为要求；实现机制；模型／输入假设 | 已实现 API 门槛；V2 |
| [ADR-Z05](#adr-z05) | swap 前阻止需要新映射 lease 的访问并等待旧 lease 排空；已有 DLU 补片可继续，Pending 转发或 0xA 仅在无需新 lease 且通过前置检查时继续。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-Z06](#adr-z06) | 缓存失效回调完成后才提交 remap；异常保留旧映射及 quiesce。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 事务框架与 Device Cache 失效已接入；V2 |
| [ADR-Z07](#adr-z07) | Zone Remapping 当前直接使用内部 API，默认工作在 Host-controlled WL 场景。 | Host-controlled模式条件：直接规定；直接API及默认场景：模型选择 | 行为要求；模型／输入假设 | 语义子集；V2 |
| [ADR-C01](#adr-c01) | 各类映射与退役状态有明确的权威持有者；集成路径经 CapacityRetirementCoordinator 统一修改并获取新 IO pin。 | 状态管理与统一变更入口：受规范约束，机制未指定；具体C++防绕过与身份检查：无直接规定 | 实现机制（状态所有权与统一变更入口）；实现保障（防绕过与身份检查） | 已实现；V1 |
| [ADR-C02](#adr-c02) | 最终解析先拒绝逻辑退役，再查 nominal slot 的 hole；未命中才走 replacement。 | 退役地址拒绝：直接规定；hole/replacement优先级：受规范约束，机制未指定 | 行为要求；实现机制 | 已实现；V1 |
| [ADR-C03](#adr-c03) | 坏块用同 channel/die/bank 单调分配的 spare 替换，跳过坏 spare，不复用退役块。 | 受规范约束，分配机制未指定 | 实现机制；模型／输入假设 | 已实现；V1 |
| [ADR-C04](#adr-c04) | 无法替换时永久标记逻辑 block，首次写报告 0x8/info1，后续读写为 0x1。 | 退役及后续错误：直接规定；修复耗尽的检测时机：受规范约束，机制未指定 | 行为要求；实现机制 | 块级内存态已实现；V2 |
| [ADR-C05](#adr-c05) | swap 只重新分配双方实际拥有的健康物理块，保持第三方所有权及逻辑退役集合。 | 正确性边界受规范约束；双边资源池是补偿研究策略的一部分 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-C06](#adr-c06) | 稀疏补偿条目直接保存最终物理块；匹配优先默认目标，再原目标，再同 lane 有序目标。 | 无直接算法规定；补偿研究策略的规范等价性尚未证明 | 实现机制 | 已实现；V2 |
| [ADR-C07](#adr-c07) | 补偿条目预算默认50‰，分母为每 channel nominal blocks，向下取整且无最小一项。 | 无直接规定 | 可配置策略；模型／输入假设 | 参数已实现；V1 |
| [ADR-C08](#adr-c08) | DensityLimit/RecoveryRequired 等失败不部分提交，并保留 quiesce，调用方需取消或重试。 | 无直接规定；内部事务契约 | 实现保障 | 已实现；V1 |
| [ADR-C09](#adr-c09) | 映射、hole、逻辑退役和 spare 分配先准备候选、再同步失效、最后统一提交，避免暴露半更新元数据。 | 跨组件一致提交：受规范约束，机制未指定，并受选定映射解释限定；具体C++异常安全与容器提交手段：无直接规定 | 实现保障（事务一致性与失败语义）；实现机制（分阶段提交） | 已实现；V2 |
| [ADR-C10](#adr-c10) | discard/remap 成功才推进逻辑 zone content generation，以识别 A→B→A 的内容失效。 | 受已选内容生命周期解释约束，机制未指定 | 实现保障 | 已实现；V1 |
| [ADR-C11](#adr-c11) | 实际 IO 同时持有 zone、nominal slot 和最终物理目标 lease，替换快照共享计数。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-C12](#adr-c12) | 不变量检查器验证无物理别名、同lane、合法范围、密度、无孤立/冗余hole。 | 无直接规定 | 实现保障 | 已实现；V2 |
| [ADR-E01](#adr-e01) | 事件按 time、round、phase、递增ID推进；同Tick先形成因果闭包，再冻结到期批次统一结算，不能用单层 phase 概括全部顺序。 | 同刻deadline裁决：规范未明确边界下的项目解释；排序结构：无直接规定 | 行为要求（项目解释）；实现保障 | 已实现；V2 |
| [ADR-E02](#adr-e02) | 首片建立固定deadline，后续补片不续期；只对Collecting超时并完成本DLU所有命令。 | 起算/固定窗口/DLU错误范围：直接规定；Collecting门控：机制未指定 | 行为要求；实现机制 | 已实现；V1 |
| [ADR-E03](#adr-e03) | 取消deadline并用context generation复核，旧事件不得终止新context；析构取消自有deadline。 | 无直接规定 | 实现保障 | 已实现；V1 |
| [ADR-E04](#adr-e04) | run_next先推进事件时间，再取出事件；invoke在回调前消费事件身份，异常不重复执行该事件。 | 无直接规定 | 实现保障 | 已实现；V1 |
| [ADR-E05](#adr-e05) | erase/program 使用一次性OperationId，完成接口才更新media；未知或重复ticket拒绝。 | Core完成前不得成功：直接规定；ticket和介质更新接口：受规范约束，机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-E06](#adr-e06) | 错误响应与不接受请求分开；当前只实现列明的读写响应子集，API 诊断不冒充协议响应，响应观察顺序不是 AXI 顺序。 | 错误码/作用域有直接依据；未接受背压边界见项目解释；协议抽象范围为模型选择 | 行为要求（项目解释）；实现机制；模型／输入假设 | 部分实现；V2 |
| [ADR-E07](#adr-e07) | 集成错误裁决遵守既有事件阶段；先生成的最终响应不被后来故障改写。 | 等时刻事件顺序：沿用规范未明确边界下的项目解释；一次最终响应与生命周期：受规范约束，机制未指定 | 行为要求（项目解释）；实现保障 | 当前事件子集已集成验证；V1 |
| [ADR-M01](#adr-m01) | 以64个DataToken表示一页内容；稀疏map保存功能介质，初始视为已擦除。 | 无直接规定 | 模型／输入假设；实现机制 | 已实现；V2 |
| [ADR-M02](#adr-m02) | 物理页只能按next_page顺序program；erase清页并重置物理进度，不清除磨损历史。 | 顺序与擦除约束有直接依据；扁平物理进度和清理机制未指定 | 实现机制；模型／输入假设；实现保障 | 已实现；V2 |
| [ADR-M03](#adr-m03) | PEC基线在成功erase时加一，磨损跟随物理块，remap/replay不转移或清零。 | PEC用于磨损管理有依据；精确记账模型未指定 | 模型／输入假设；实现机制 | 已实现；V1 |
| [ADR-M04](#adr-m04) | 固定时延、演示几何和 Tick 配置用于功能执行，不作为已校准的 HBF 性能或硬件成本参数。 | 无直接规定 | 模型／输入假设 | 演示模型已实现；V0（参数校准未做） |
| [ADR-D01](#adr-d01) | Batch hint立即关闭当前读窗口：R R B B R → [R R B] [B]，末尾R等timer。 | 规范歧义下的项目解释 | 行为要求（项目解释） | 已集成 Device 读窗口；V2 |
| [ADR-D02](#adr-d02) | Regular read窗口策略支持固定timer或队列深度阈值，加timer兜底。 | 受规范约束，调度策略未指定 | 可配置策略 | 接口、策略及主动 timer 已集成；V2 |
| [ADR-D03](#adr-d03) | WearLevelingPolicy只能建议交换pair，正确性由映射组件检查。 | 受规范约束，策略未指定 | 可配置策略（接口预留）；实现保障 | 仅接口；V0 |
| [ADR-D04](#adr-d04) | 最小 ReadPath 接入 64B 读及 0x0/0x1/0x2/0x3/0x7/0xA 响应，以 DataToken 表示数据。 | 状态含义与基本读粒度：直接规定；接入方式：受规范约束，机制未指定；支持子集与 token 抽象：无直接规定 | 行为要求；实现机制；模型／输入假设 | 最小功能路径已实现；V2（相对于完整规范读路径） |
| [ADR-D05](#adr-d05) | 当前内容代的 Pending DLU 优先提供已收片段；目标片段缺失返回 0xA，不回退旧 NAND。 | Pending 转发与缺片段结果：直接规定；与恢复限制组合的优先级：规范未明确边界下的项目解释；查询组织：受规范约束，机制未指定 | 行为要求；行为要求（项目解释）；实现机制 | 已接入最小读路径；V1 |
| [ADR-D06](#adr-d06) | 当前内容处于恢复阶段时，Pending处理之后的其余读返回0x2；合并终点program成功后放行。 | 0x2 类型及允许产品定义限制原因/持续条件：直接规定；本项目选用的恢复限制策略：受规范约束，机制未指定，并依赖既定 replay 解释 | 行为要求；实现机制（固定策略） | 固定当前策略已实现；V1（给定所选恢复解释） |
| [ADR-D07](#adr-d07) | 读限制从 ReplayState、准备状态、内容代及物理/逻辑事实推导，不另存可漂移的 blocked 状态。 | 受选定恢复解释与规范相关行为约束，机制未指定 | 实现机制；实现保障 | 已实现派生查询；V1 |
| [ADR-D08](#adr-d08) | 逻辑退役读报 0x1；仅已知物理目标不可用时按当前策略报 0x2，不推断 ECC 或新退役。 | 已退役地址 0x1 与首次不可恢复容量通知含义：直接规定；检测时机与临时读限制选择：受规范约束，机制未指定 | 行为要求；实现机制（固定策略） | 块级事实分流已接入；V1；读主动退役与广域异步退役未实现 |
| [ADR-D09](#adr-d09) | 介质读在 submit 时只登记候选；issue 时重新检查并绑定当前目标、采样结果，finish_read 再发布稳定结果。 | 读 issue 时刻与结果快照：无直接规定，属于当前功能模型假设；结果生命周期：受规范相关响应约束，机制未指定 | 模型／输入假设；实现机制 | 固定功能模型已实现；V1 |
| [ADR-D10](#adr-d10) | 读 ticket 固定目标到完成；有界结果所有权与 MOCS credit 保留至 Host 交付。 | 每 command 一次响应：直接规定；lease 与响应副本：受规范约束，机制未指定；独立 ID 与关闭清理：无直接规定 | 行为要求；实现机制；实现保障 | 已实现；V1 |
| [ADR-D11](#adr-d11) | 内部 erase 或映射门禁采用未接受的 Backpressured；已有 Pending 转发与补片继续排空。 | 内部等待的接收边界：规范未明确边界下的项目解释；lease 门禁及排空：受规范相关约束，机制未指定 | 行为要求（项目解释）；实现机制；实现保障 | 背压、RX 重试与候选等待已接入；V1 |
| [ADR-D12](#adr-d12) | discard/remap 后以新内容代隔离旧数据；尚未绑定新内容的防御性读返回 0x7。 | 旧内容隔离：受规范约束，机制未指定；禁止访问期间的防御性 0x7：无直接规定，属于模型边界选择 | 实现机制；模型／输入假设 | 防御性读取及内容代隔离已实现；V1；损坏目标 remap 仍有限制 |
| [ADR-LOOP-E01](#adr-loop-e01) | 同一Tick内用因果轮次推进零延迟反馈，不随意增加Tick。 | 无直接规定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-E02](#adr-loop-e02) | 截止Tick先处理正常因果闭包；只有正式接受并进入组装的补片才能救回DLU。 | 规范歧义或未明确边界下的项目解释 | 行为要求（项目解释） | 已实现于当前子集；V1 |
| [ADR-LOOP-E03](#adr-loop-e03) | 同Tick组装deadline冻结为一批，整批终结后才推进其反馈。 | 规范歧义或未明确边界下的项目解释 | 行为要求（项目解释）；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-E04](#adr-loop-e04) | 事件取消、候选身份和窗口timer版本阻止迟到工作作用于新上下文。 | 无直接规定 | 实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-P01](#adr-loop-p01) | 读接受只创建候选；显式issue才发射，完成与响应交付继续分开。 | 受规范约束，机制未指定 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-P02](#adr-loop-p02) | 已接受未发射读持逻辑内容依赖，冲突重写/repair/remap需等待；已接受读可穿过后来的quiesce排空。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-P03](#adr-loop-p03) | 介质读在issue时绑定当前物理目标并采样符号内容，完成时稳定结果并释放保护。 | 绑定/保存：受规范约束，机制未指定；采样时刻：无直接规定 | 实现机制；模型／输入假设 | 已实现于当前子集；V1 |
| [ADR-LOOP-P04](#adr-loop-p04) | 排队读遇到恢复/不可用等新事实时重查并固定错误，释放旧内容依赖，不无限等待修复。 | 受规范约束，机制未指定（受已选恢复读解释约束） | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-P05](#adr-loop-p05) | erase/program候选查询不改变业务状态；发射前复核路径条件并取得一次性ticket。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-P06](#adr-loop-p06) | 成功读结果由有界存储持有，路径只持可移交句柄。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-P07](#adr-loop-p07) | read_group 按 (channel, group) 保持返回顺序；组头普通读才允许取数/发射，后续成员可先进入 RX/MOCS，错误也推进组。 | 同ID有序：直接规定；显式组与组头执行：受规范约束，机制未指定 | 行为要求；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-P08](#adr-loop-p08) | 有序Pending成功转发/0xA组合仍报LIMIT-READ-001模型能力诊断。 | 无直接规定 | 模型／输入假设；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-P09](#adr-loop-p09) | 按原因和对象一次性订阅，版本复核避免漏唤醒，重复通知合并，回调后重查。 | 无直接规定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-P10](#adr-loop-p10) | 物理lease析构只更新计数与dirty标记，在稳定边界统一发布释放通知。 | 无直接规定 | 实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-R01](#adr-loop-r01) | 显式Core Channel路由接在UCIe与Die之间，当前无限带宽、零延迟、不争用。 | 拓扑受规范约束；无限带宽/零延迟无直接规定 | 模型／输入假设；实现机制 | 已实现于当前子集；V2 |
| [ADR-LOOP-R02](#adr-loop-r02) | 每个物理Bank同一时刻最多一个read/program/erase，独立Bank可并行；不全程锁祖先Die。 | 受规范约束，机制未指定（保守资源基线） | 模型／输入假设；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-R03](#adr-loop-r03) | NAND选择、UCIe响应仲裁和窗口释放分属不同策略，Device先过滤合法候选。 | 受规范约束，机制未指定 | 可配置策略；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-R04](#adr-loop-r04) | 同Bank sense严格有序；实现以控制器正式接受序为基线检查较早未满足读。 | 严格sense顺序：直接规定；采用正式接受序：项目解释 | 行为要求；行为要求（项目解释） | 已实现于当前子集；V1 |
| [ADR-LOOP-R05](#adr-loop-r05) | 每Bank两个页Cache；合法命中不等待读窗口，发射sense前再次查Cache。 | 双缓存和及时命中服务：直接规定；查询流水组织：受规范约束，机制未指定 | 行为要求；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-R06](#adr-loop-r06) | Cache替换仅选择未填充、未被结果引用的槽，无效槽优先，随后选最久未用。 | 受规范约束，机制未指定 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-R07](#adr-loop-r07) | sense必须先有Cache目的槽；完成释放Bank，结果经有限Egress交接后才释放Cache引用。 | 受规范约束，机制未指定 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-R08](#adr-loop-r08) | erase/program/failure/remap改变介质时失效Cache标签，但保留受保护的已确定结果。 | 受规范约束，机制未指定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-W01](#adr-loop-w01) | 每UCIe channel一个读累积窗口，可覆盖其下多个Die/Bank。 | 受规范约束，机制未指定 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-W02](#adr-loop-w02) | 接受B立即关闭截至自身的当前集合；已转发或0xA的有效B也触发边界，NotTaken/未接受无副作用。 | 规范歧义或未明确边界下的项目解释 | 行为要求（项目解释）；实现机制 | 已实现于当前子集；V2 |
| [ADR-LOOP-W03](#adr-loop-w03) | 窗口释放的是调度资格；成员可分时发射完成，不要求整批同时完成或跨集合屏障。 | 受规范约束，机制未指定 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-W04](#adr-loop-w04) | 窗口用主动timer和深度阈值推进；成员变化取消/重建timer，旧版本不释放新窗口。 | 累积/调度受规范约束；主动事件与版本属于实现保障 | 可配置策略；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-L01](#adr-loop-l01) | Taken时预留RX并开始传输，完整到达转驻留；源在NotTaken时继续持有请求。 | 流控/缓冲受规范约束，机制未指定；预留边界为模型选择 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-L02](#adr-loop-l02) | RX 驻留不占 MOCS；正式路径接受后才记 credit。建模的 Host 语义错误进入 MOCS 后等待其响应 credit，API 能力/形状诊断在入口直接报错。 | 计数对象受规范约束；接收门限为已选项目解释 | 实现机制；行为要求（项目解释） | 已实现于当前子集；V1 |
| [ADR-LOOP-L03](#adr-loop-l03) | 写metadata和DataToken分开存放，但Host一次提交关联事务，不拆成AW/W两条命令。 | 无直接规定（事务级模型抽象） | 模型／输入假设；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-L04](#adr-loop-l04) | 使用UCIe有效GB/s计算序列化服务时间；速率按物理channel预算，不乘虚拟AXI接口数。 | 速率档来源：直接规定；事务成本/独立方向映射：模型假设 | 行为要求；模型／输入假设 | 已实现于当前子集；V2 |
| [ADR-LOOP-L05](#adr-loop-l05) | 序列化结束即可服务下一事务，传播延迟可重叠；缓冲保存到对应到达/交付边界。 | 无直接规定（时延分解模型） | 模型／输入假设；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-L06](#adr-loop-l06) | RX/TX都有限，条目与payload字节分开记账；成功数据和元数据响应均占返回条目。 | 有限适配器有依据；聚合预算和深度无直接规定 | 实现机制；模型／输入假设 | 已实现于当前子集；V2 |
| [ADR-LOOP-L07](#adr-loop-l07) | 交付恰好一次释放对应credit/返回槽，Host反馈在后续同Tick事件执行；读写响应保留独立类型。 | 一次响应受规范约束；后续事件回调与variant为实现机制 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-L08](#adr-loop-l08) | Pending在正式接受侧观察；成功转发先预留结果空间，未接受可重查，接受后成功/0xA不随补片改变。 | Pending结果：直接规定；观察/预留边界：受规范约束，机制未指定 | 行为要求；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-H01](#adr-loop-h01) | 有限Global范围先按字节地址交织，再拆为local逻辑片段；多个Global条目可共同补齐DLU。 | 映射受规范直接公式约束；范围驱动/汇总机制未指定 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-H02](#adr-loop-h02) | 默认64B写命令；只有显式burst模式才合成完整同local DLU写事务，并保持各Global子工作归属。 | 命令/页条件受规范约束；默认拆分和聚合为Host策略 | 可配置策略；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-H03](#adr-loop-h03) | 输入中的额外Write完成依赖直接拒绝；支持Read等待Global最终成功。 | 无直接规定 | 模型／输入假设 | 已实现于当前子集；V1 |
| [ADR-LOOP-H04](#adr-loop-h04) | content标识不可变逻辑内容；同块内容不A→B→A回用，同内容同64B片段不能重复出现在输入。 | 无直接规定 | 模型／输入假设；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H05](#adr-loop-h05) | Host按channel维护待发送集合，已Taken事务移出集合；补齐打开DLU优先，并限制打开数量。 | 无直接规定（Host发送策略） | 可配置策略；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-H06](#adr-loop-h06) | Host发送策略只选择已过滤合法候选，不能直接修改设备或跳过依赖；默认选择确定。 | 受规范约束，机制未指定 | 可配置策略；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H07](#adr-loop-h07) | Global读提示提供AllRegular与每范围每channel尾Batch两种配置；尾B不越过同范围未接受前序。 | 受规范约束，机制未指定 | 可配置策略 | 已实现于当前子集；V1 |
| [ADR-LOOP-H08](#adr-loop-h08) | Host区分Global工作、64B逻辑片段、发送单元、命令尝试与恢复任务身份。 | 无直接规定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H09](#adr-loop-h09) | Host从Taken到最终反馈消费持续跟踪同一尝试，不在Link/RX重试时重新发送。 | 无直接规定（源端所有权协议） | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H10](#adr-loop-h10) | 正式接受通知与最终响应分开；接受通知仅推进Host补片进度和replay计数。 | 无直接规定（模型公开进度接口） | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H11](#adr-loop-h11) | Host从0x7与自己的原命令地址定位失败页；0x6/info2可先到，但不能猜根因页。 | 错误含义：直接规定；Host关联等待机制未指定 | 行为要求；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-H12](#adr-loop-h12) | 每遍replay前冻结同块旧已移交集合，等其全部最终归属；新读不扩大集合，其他块不需清零。 | 受选定恢复解释约束，机制未指定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H13](#adr-loop-h13) | Host有限保留区按内容版本保存原Global地址和token；完整恢复前缀通过核验后固定，恢复结束才解固定。 | 受恢复内容要求约束，机制未指定；容量/FIFO为模型选择 | 实现机制；模型／输入假设 | 已实现于当前子集；V1 |
| [ADR-LOOP-H14](#adr-loop-h14) | 同次失败的多响应只建立一个恢复任务；再次失败仍沿用任务，并保留最远未完成终点。 | 受原恢复范围项目解释约束，机制未指定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H15](#adr-loop-h15) | 同一恢复任务默认最多3遍，首条page0正式接受时计本遍一次；合并失败不重置预算。 | 无直接规定 | 可配置策略；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H16](#adr-loop-h16) | 当前replay逐页推进，每页补齐后等全部成功响应；终点完成后重发此前失败的后继普通页。 | 写序受规范约束；逐页等待是保守Host机制 | 实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-H17](#adr-loop-h17) | 数据不足/预算耗尽结束相关逻辑任务，继续排空旧命令；不清空设备恢复义务、不自动退役。 | 无直接规定（Host失败归属和退出策略） | 实现机制；模型／输入假设 | 已实现于当前子集；V1 |
| [ADR-LOOP-H18](#adr-loop-h18) | 恢复期间普通读照常按Device结果结束；显式依赖成功的读才由Host推迟，不自动循环重试0x2。 | 0x2含义受规范约束；Host发送/重试策略无直接规定 | 可配置策略（输入依赖）；实现机制 | 已实现于当前子集；V1 |
| [ADR-LOOP-H19](#adr-loop-h19) | Global条目等所有逻辑片段最终归属后结束；分别统计尝试错误与恢复后逻辑成功。 | 无直接规定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-H20](#adr-loop-h20) | Host接受bitmap为暂定发送进度；错误按失败范围撤销，页级修复状态独立于Device组装位图。 | 写序与响应受规范约束；进度修正机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-LOOP-H21](#adr-loop-h21) | Pending满只重发失败命令，保留有效补片，不以整个block排空作为重试前提。 | 错误范围受规范约束；自动重试机制未指定 | 实现机制；可配置策略 | 已实现；V1 |
| [ADR-LOOP-H22](#adr-loop-h22) | Pending重试由容量相关完成反馈或正时间间隔timer触发，提示不保证资源可用。 | 完成后再开DLU有依据；反馈代理及timer为无直接规定的模型选择 | 实现机制；可配置策略；模型／输入假设 | 已实现；V1（执行）；V0（参数校准） |
| [ADR-LOOP-H23](#adr-loop-h23) | timeout修复先排空同页旧尝试、检查完整数据及释放时刻，再重发整个DLU。 | 整DLU重发被明确允许；排空与数据检查机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-LOOP-H24](#adr-loop-h24) | 只修复能关联前页资源错误的0x6/info1，根因可后到；后继重发保守地等待前页成功。 | 受写序约束，关联及等待机制未指定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-LOOP-H25](#adr-loop-h25) | Pending命令重试与timeout整页重试分别设有限预算，0可禁用；耗尽或缺数据明确失败并排空。 | 无直接规定 | 可配置策略；实现保障 | 已实现；V1（边界）；V0（参数校准） |
| [ADR-LOOP-H26](#adr-loop-h26) | 页级资源修复与program-fail replay分开；replay内资源重试不另建任务或增加遍数。 | 受既定恢复解释约束，机制未指定 | 实现机制；实现保障 | 已实现；V1（已列交叉场景） |
| [ADR-LOOP-F01](#adr-loop-f01) | Completed须Host和Device临时活动全部排空；逻辑成功/失败另报，保留持久介质/映射/PEC/有效Cache。 | 无直接规定 | 实现机制；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-F02](#adr-loop-f02) | 有未来timeout/窗口/操作事件就继续跳时；仅无有效事件且仍有工作才Stalled，被动订阅不算未来进展。 | 无直接规定 | 实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-F03](#adr-loop-f03) | 时间/总callback/同Tick callback/轮次数有独立限额，LimitReached保留现场；能力诊断与内部错误分开。 | 无直接规定 | 可配置策略；实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-F04](#adr-loop-f04) | 依赖失败取消未来工作后，已无用途的release事件也取消，避免虚假时间限额退出。 | 无直接规定 | 实现保障 | 已实现于当前子集；V1 |
| [ADR-LOOP-M01](#adr-loop-m01) | 统一Tick为1ps；固定介质/缓存延迟仅用于功能执行，未校准。 | 无直接规定 | 模型／输入假设；可配置策略（参数） | 已实现于当前子集；V0（物理校准）；参数执行路径有测试 |
| [ADR-LOOP-M02](#adr-loop-m02) | 缓冲、Host保留容量和演示几何独立配置，默认值不称为规范给定容量。 | 无直接规定 | 模型／输入假设 | 已实现于当前子集；V2（执行约束）；物理依据未验证 |
| [ADR-LOOP-M03](#adr-loop-m03) | 文本输入版本化且解析器独立于核心；先完整校验再发请求，CLI报告实际配置与逻辑/尝试结果。 | 无直接规定 | 模型／输入假设；实现保障 | 已实现于当前子集；V1 |
| [ADR-STAT-01](#adr-stat-01) | 统一记录释放、移交、RX到达、正式接受、响应交付；使用事件发生时刻。 | 无直接规定 | 模型／输入假设；实现保障 | 已实现；V1 |
| [ADR-STAT-02](#adr-stat-02) | Global、原始64B逻辑工作和命令尝试分别统计；恢复不重置逻辑时刻或扩大分母。 | 无直接规定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-STAT-03](#adr-stat-03) | 统计只观察，不驱动执行；缺失时刻和未完成记录不伪装成已完成样本。 | 无直接规定 | 实现机制；实现保障 | 已实现；V1 |

| [ADR-CFG-01](#adr-cfg-01) | 统一配置按默认值、文件、显式CLI覆盖装配，校验后冻结并向组件传递子配置。 | 无直接规定 | 实现机制；实现保障 | 已实现；V1 |
| [ADR-CFG-02](#adr-cfg-02) | Speed Grade派生有效带宽和结构上限；控制服务量及固定传播延迟单列为模型假定。 | 档位表直接规定；模型档位与几何维度关联、事务计费机制由项目选择 | 行为要求；模型／输入假设；实现保障 | 已实现；V1 |
| [ADR-CFG-03](#adr-cfg-03) | 策略名称与参数统一配置；仅装配已实现策略，不忽略未知或不适用的设置。 | 策略受规范约束；配置机制无直接规定 | 可配置策略；实现机制；实现保障 | 已实现；V1 |
| [ADR-CFG-04](#adr-cfg-04) | 有效配置可导出并重载；Core拓扑、容量预算和派生容量使用同一组冻结参数。 | 无直接规定 | 实现机制；模型／输入假设；实现保障 | 已实现；V1 |

### 3.1 跨组件决策关系

| 主题 | 关联决策 | 职责边界 |
|---|---|---|
| 事件和 deadline | [ADR-E01](#adr-e01)、[ADR-LOOP-E01](#adr-loop-e01)、[ADR-LOOP-E02](#adr-loop-e02)、[ADR-LOOP-E03](#adr-loop-e03) | 同 Tick 因果轮次、正常事件闭包和 deadline 批结算共同决定执行顺序。 |
| 读生命周期 | [ADR-D09](#adr-d09)、[ADR-LOOP-P01](#adr-loop-p01)、[ADR-LOOP-P03](#adr-loop-p03) | 接受登记候选；实际 issue 绑定物理目标；完成发布稳定结果。 |
| 内容与结果保护 | [ADR-D10](#adr-d10)、[ADR-C11](#adr-c11)、[ADR-LOOP-P06](#adr-loop-p06)、[ADR-LOOP-R07](#adr-loop-r07) | 逻辑依赖、物理 lease 和 Cache/Egress 结果引用有不同释放边界。 |
| 读窗口 | [ADR-D01](#adr-d01)、[ADR-D02](#adr-d02)、[ADR-LOOP-W01](#adr-loop-w01)、[ADR-LOOP-W02](#adr-loop-w02)、[ADR-LOOP-W04](#adr-loop-w04) | 每 channel 累积读；Batch、阈值和主动 timer 释放调度资格。 |
| 接收与交付 | [ADR-H04](#adr-h04)、[ADR-H08](#adr-h08)、[ADR-LOOP-L01](#adr-loop-l01)、[ADR-LOOP-L02](#adr-loop-l02)、[ADR-LOOP-L07](#adr-loop-l07) | 链路/RX 移交、正式接受和 MOCS 释放分别记账。 |
| Global 拆分 | [ADR-A03](#adr-a03)、[ADR-LOOP-H01](#adr-loop-h01)、[ADR-LOOP-H02](#adr-loop-h02) | 单地址交织扩展为范围拆分，并保留跨 Global 补齐 DLU 的工作归属。 |
| 恢复 | [ADR-R02](#adr-r02)、[ADR-R03](#adr-r03)、[ADR-R05](#adr-r05)、[ADR-LOOP-H11](#adr-loop-h11)、[ADR-LOOP-H12](#adr-loop-h12)、[ADR-LOOP-H13](#adr-loop-h13) | Device 约束恢复合法性；Host 根据响应和自身保留数据组织 replay。 |
| Host写重试 | [ADR-LOOP-H10](#adr-loop-h10)、[ADR-LOOP-H20](#adr-loop-h20)、[ADR-LOOP-H21](#adr-loop-h21)、[ADR-LOOP-H23](#adr-loop-h23)、[ADR-LOOP-H24](#adr-loop-h24)、[ADR-LOOP-H26](#adr-loop-h26) | 正式接受不代表Assembly保留；Host修正发送进度，按错误范围重试，Device继续独立检查合法性。 |
| Pending 观察 | [ADR-D05](#adr-d05)、[ADR-LOOP-P08](#adr-loop-p08)、[ADR-LOOP-L08](#adr-loop-l08) | 正式接受固定数据或错误结果；显式有序 Pending 组合不在支持范围内。 |
| 参数和模型范围 | [ADR-M01](#adr-m01)、[ADR-M04](#adr-m04)、[ADR-LOOP-M01](#adr-loop-m01)、[ADR-LOOP-M02](#adr-loop-m02) | 符号数据、固定时延和容量参数用于功能执行，尚未硬件校准。 |

## 4. 逐条记录

以下按主题组织决策。ADR 标识用于关联要求、实现和测试；相邻条目可能分别描述同一行为的规范要求与实现机制。

### A. 地址与静态描述

<a id="adr-a01"></a>

#### ADR-A01｜Host 粒度为 64B，Core 写入及 DLU 为 4KiB；一 DLU 含 64 个片段。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定；片段数为直接推导 |
| 条目性质 | 行为要求 |
| 分类边界说明 | 64B 与 4KiB 的适用操作范围以规范依据及 ADR-H01 为准，不能理解成所有读写 command 都只能长 64B。4096/64=64 是算术结果；常量定义就是要求的实现位置，无须另列机制条目。 |
| 规范依据/无直接依据说明 | §4.1 p15、§4.6 p23、§5.3.1 p56、§5.4.1(3)(6) p58，直接规定粒度。 |
| 关联要求/条目 | REQ-ADDR、REQ-ASSEMBLY |
| 选择理由 | 规范直接要求；类型及常量形式属于内部实现。 |
| 模型假设/参数依据 | 64、4096 来自规范；token 抽象见 ADR-M01。 |
| 实现位置（成员/函数） | [host_unit_bytes / dlu_bytes / fragments_per_dlu](../include/hbf_eventlab/common/types.hpp)；[normalize_command() 生成 slot](../src/runtime/command_admission.cpp) |
| 验证测试与覆盖 | [T02](verification.md#t02) 检查 4KiB 拆为 64 片段；[T03](verification.md#t03) 检查收齐、转发、父命令去重。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | DataToken 只代表一个 64B 单元的数据身份，不是实际 64 字节载荷。 |

<a id="adr-a02"></a>

#### ADR-A02｜内部 LocalByteAddress 是通道 Flash 区域零起点字节偏移；64B 编号另设类型并显式换算。

| 字段 | 记录 |
|---|---|
| 规范关系 | 内部字节偏移：受规范约束，表示未指定；强类型：无直接规定 |
| 条目性质 | 模型／输入假设；实现保障 |
| 分类边界说明 | 图3的64B地址单位是关联的外部要求。内部以Flash区域零起点字节偏移表示地址是项目约定；LocalByteAddress/LocalAddress64B及显式换算是防止单位混用的保障。内部零起点不要求FIOSA为0。 |
| 规范依据/无直接依据说明 | §4.6 图3 p22 使用 64B 单位；§5.9.2.9 p74 定义 FIOSA。规范没有要求模拟器使用这些 C++ 类型。 |
| 关联要求/条目 | REQ-ADDR、[ADR-A01](#adr-a01) |
| 选择理由 | 避免 Byte Address、64B Address、DLU Index 混用或重复减去起点。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [GlobalByteAddress / LocalByteAddress / LocalAddress64B](../include/hbf_eventlab/common/types.hpp)；[to_bytes() / to_units64()](../include/hbf_eventlab/common/types.hpp) |
| 验证测试与覆盖 | [T11](verification.md#t11) 检查 64B↔byte 换算和不对齐拒绝；[T01](verification.md#t01) 检查 global base。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 没有 FIOSA 协议适配层；不能把内部零起点解释为硬件必须 FIOSA=0。 |

<a id="adr-a03"></a>

#### ADR-A03｜Host 地址按条带轮转到 channel，并移除 global base；条带内偏移不变。

| 字段 | 记录 |
|---|---|
| 规范关系 | 映射公式：直接规定；条带内偏移保持：直接推导；global base适配：无直接规定 |
| 条目性质 | 行为要求；模型／输入假设 |
| 分类边界说明 | 在规范适用配置和一致单位下，map()中的channel/local计算直接实现图3；减去global base是系统字节地址输入契约，不是FIOSA处理。输入若已经是HBF相对地址就不能重复减base。类名及封装方式不单独分类；超出规范配置范围的推广不随公式一起获得规范依据。 |
| 规范依据/无直接依据说明 | §4.6 图3 p22 提供 channel/local 公式及可配置交织粒度；实现将公式转换成字节单位并推广 channel 数。 |
| 关联要求/条目 | REQ-ADDR、[ADR-A02](#adr-a02) |
| 选择理由 | 公式保持一致；global base 属于 Host 全局映射，不是 FIOSA。 |
| 模型假设/参数依据 | base/channels/capacity/split 可配置；只对与所选 OCP 配置相符的输入主张公式对应。 |
| 实现位置（成员/函数） | [RoundRobinInterleaver::构造函数 base_/split_bytes_](../src/policies/round_robin_interleaver.cpp)；[RoundRobinInterleaver::map()](../src/policies/round_robin_interleaver.cpp)；[IHostInterleaver::map()](../include/hbf_eventlab/interfaces/address.hpp) |
| 验证测试与覆盖 | [T01](verification.md#t01) 只直接覆盖两通道、64B 条带、非零 base 及低于 base 的地址；其他配置未穷举。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 只映射一个地址，不拆分跨通道 workload range；不模拟多 AXI port。构造函数允许任意合法 64B 倍数 split，图3的 2^n 范围与推广部分需区分。 |

<a id="adr-a04"></a>

#### ADR-A04｜按 bank→die→page→block 解码 DLU；encode_logical 是其逻辑视图的逆变换。

| 字段 | 记录 |
|---|---|
| 规范关系 | 图示bank/die条带：直接规定；完整扁平布局与逆变换：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制；模型／输入假设 |
| 分类边界说明 | 区分图4中的DLU条带关系与项目的bank→die→page→block扁平表示。encode/decode往返是所选表示的正确性条件；并不意味着规范规定这个类、任意几何或省略wordline/string的完整布局。 |
| 规范依据/无直接依据说明 | §4.6 图4 p23 展示 bank/die DLU striping；§5.7 p63 给出 block 起点及步进计算。不能直接把 p22 的“低位”解释成字节地址低位。 |
| 关联要求/条目 | REQ-ADDR、[ADR-A01](#adr-a01) |
| 选择理由 | REFERENCE 布局；将扁平 page 编号作为当前可执行模型，避免把中间槽位误称最终物理地址。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [ReferenceAddressLayout::decode()](../src/policies/reference_address_layout.cpp)；[logical_view()](../src/policies/reference_address_layout.cpp)；[encode_logical()](../src/policies/reference_address_layout.cpp) |
| 验证测试与覆盖 | [T01](verification.md#t01) 遍历示例 channel 的全部 DLU 做 encode/decode 往返及 remap 后 page/lane 保持；[T11](verification.md#t11) 检查多层解析。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 未显式建模 wordline/string、R5 独立条带宽度或完整物理拓扑；任意自定义布局并未被证明符合规范。 |

<a id="adr-a05"></a>

#### ADR-A05｜逻辑块、nominal slot、最终 physical block 分别建模；顺序状态使用逻辑块键。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | Host逻辑坐标与动态物理目标的语义需要保持；三类地址类型及以LogicalBlockKey保存顺序状态是实现该边界的机制。不能仅因有类型封装就把所有关联地址要求改为实现选择。 |
| 规范依据/无直接依据说明 | 规范没有这些类型要求；受 §4.6、§5.7 和 remap 后地址解释约束。 |
| 关联要求/条目 | REQ-ADDR、REQ-REPLAY、REQ-ZONE |
| 选择理由 | 区分 Host 顺序/恢复坐标与动态映射目标，避免备用块编号进入 Host replay 计算。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [LogicalBlockKey / LogicalNandAddress / NandSlotAddress](../include/hbf_eventlab/common/types.hpp)；[BlockSlotKey / PhysicalBlockKey / PhysicalNandAddress](../include/hbf_eventlab/common/types.hpp)；[WritePath::blocks_ / bindings_](../include/hbf_eventlab/runtime/write_path.hpp) |
| 验证测试与覆盖 | [T11](verification.md#t11)、[T19](verification.md#t19)、[T20](verification.md#t20) 检查替换、remap、ABA 和补偿后逻辑/物理语义。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 类型区分不保证自定义 IAddressLayout 的语义正确；WritePath 仍要求 page-preserving 布局。 |

<a id="adr-a06"></a>

#### ADR-A06｜Geometry、Capabilities、DeviceOperatingConfiguration 在 StaticDeviceDescription 构造后冻结。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定；属于当前模拟范围选择 |
| 条目性质 | 模型／输入假设 |
| 分类边界说明 | 规范中的能力/配置语义不要求构造后冻结。冻结体现当前不支持运行时CSR/ADMIN配置修改；const getter只是落实这一范围的代码。 |
| 规范依据/无直接依据说明 | §5.9有能力/配置寄存器；冻结并不由规范要求，是当前不模拟运行时CSR修改的范围选择。 |
| 关联要求/条目 | REQ-ADDR、REQ-CAPACITY |
| 选择理由 | 当前不支持运行时 CSR/ADMIN 配置修改；const view 保持单次模拟内的静态描述一致。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [StaticDeviceDescription::geometry_/capabilities_/configuration_ 及 const getters](../include/hbf_eventlab/architecture/device_description.hpp) |
| 验证测试与覆盖 | 接口/所有权静态检查；[T01](verification.md#t01)/T02使用该profile，但没有运行时配置更新测试。 |
| 实现状态/验证状态 | 已实现静态描述；V0 |
| 已知限制 | 不存在运行时配置变更/通知流程；多个组件仍需调用者共享一致描述。 |

<a id="adr-a07"></a>

#### ADR-A07｜容量、时间及身份编号的关键加乘操作检查 uint64 溢出。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | uint64溢出检查维护宿主程序的数值与身份正确性，不是OCP要求特定整数宽度或检查函数。 |
| 规范依据/无直接依据说明 | 无直接规范依据，属于模拟器宿主算术正确性。 |
| 关联要求/条目 | [ADR-A02](#adr-a02)、[ADR-E01](#adr-e01)、[ADR-C08](#adr-c08) |
| 选择理由 | 防止地址/代次/时间绕回造成别名和旧事件误命中。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [checked_add() / checked_mul()](../include/hbf_eventlab/common/types.hpp)；[hole_limit() 分解乘法](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | 静态检查；现有测试未发现 UINT64_MAX 等专门溢出边界用例。 |
| 实现状态/验证状态 | 已实现；V0 |
| 已知限制 | 不是对全部 C++ 算术、宿主内存分配和极限规模的形式化证明。 |

<a id="adr-a08"></a>

#### ADR-A08｜几何检查channel上限16、die数为1/2/4/8/16；其余维度允许小型测试值。

| 字段 | 记录 |
|---|---|
| 规范关系 | channel/die边界：直接规定；其余小型几何：无直接规定 |
| 条目性质 | 行为要求；模型／输入假设 |
| 分类边界说明 | 规范限定的channel上限和die枚举，与为测试允许其他维度取小正数，是不同依据的内容；后者不因共用Geometry::validate()而成为规范认可的产品配置。 |
| 规范依据/无直接依据说明 | §4.3 p17 channel上限；BUCCAP.NCDU §5.9.2.1 p69–70提供die编码。 |
| 关联要求/条目 | REQ-ADDR |
| 选择理由 | 前述边界有规范依据；正数检查是基本模型条件。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [Geometry::validate()](../src/architecture/device_description.cpp) |
| 验证测试与覆盖 | 没有专门逐一检查host_channels/dies_per_channel边界的测试；现有profile使用合法例子。 |
| 实现状态/验证状态 | 已实现部分profile校验；V0 |
| 已知限制 | bank/page/block等任意正数不等于产品规范批准；小几何是测试抽象。 |

<a id="adr-a09"></a>

#### ADR-A09｜spare为额外物理块，Host容量只计算nominal blocks。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 备用容量与Host可寻址容量分离服务于坏块管理；spare_blocks_per_bank、编号位于nominal之后及容量计算方式是本模型的表示，不能当作厂商物理备用区布局。 |
| 规范依据/无直接依据说明 | §11.5.2.8 p123描述bank备用块；没有规定代码编号分区方式。 |
| 关联要求/条目 | REQ-CAPACITY、[ADR-C03](#adr-c03) |
| 选择理由 | 逻辑容量与冗余资源分离；spare编号放在nominal之后是REFERENCE选择。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [Geometry::spare_blocks_per_bank / physical_blocks_per_bank()](../include/hbf_eventlab/architecture/device_description.hpp)；[channel_dlus()/channel_bytes()](../src/architecture/device_description.cpp)；[encode_logical() 拒绝spare编号](../src/policies/reference_address_layout.cpp) |
| 验证测试与覆盖 | [T11](verification.md#t11)/T15观察spare编号；无专门Host请求命中全部spare地址范围的协议测试。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | Host越界仍是诊断异常，非完整0x1适配；不模拟厂商真实备用区布局。 |

<a id="adr-a10"></a>

#### ADR-A10｜块替换层统一使用 BlockReplacement；无替换记录时解析到默认物理块。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 用同一具体实现处理默认目标和替换目标是代码组织选择，不是规范指定的类结构；坏块替换的行为约束与分配基线见 [ADR-C03](#adr-c03)。 |
| 规范依据/无直接依据说明 | 规范没有要求单独提供坐标直通实现。内部坏块管理的依据见 §4.1；不能以直接复制坐标绕过已提交的替换关系。 |
| 关联要求/条目 | [ADR-A05](#adr-a05)、[ADR-C02](#adr-c02)、[ADR-C03](#adr-c03) |
| 选择理由 | BlockReplacement 在没有替换记录时已经返回默认物理块，存在记录时查询替换目标；统一这两种情况，避免另一个直通实现忽略坏块替换关系。 |
| 模型假设/参数依据 | 本条无独立数值参数；默认目标的 block 编号与 nominal slot 一致，替换后的目标由 entries 保存。 |
| 实现位置（成员/函数） | [BlockReplacement::translate() 的 entries 查询与默认目标](../src/runtime/block_replacement.cpp#L30)；[CapacityRetirementCoordinator 接收具体 BlockReplacement](../include/hbf_eventlab/runtime/capacity_retirement_coordinator.hpp#L15)；[IBlockReplacement::translate()](../include/hbf_eventlab/interfaces/address.hpp#L23) |
| 验证测试与覆盖 | [T11](verification.md#t11) 检查替换前后的物理解析及读写路径；[T07](verification.md#t07) 检查坏 spare 与提交原子性；[T15](verification.md#t15) 检查替换失败及备用块耗尽。 |
| 实现状态/验证状态 | 已接入主线；V1 |
| 已知限制 | IBlockReplacement 当前仅定义地址转换接口；主线协调组件依赖具体 BlockReplacement 的计划、提交与 lease 能力，尚不能仅通过替换该接口实现切换完整坏块管理机制。完整 I/O 目标还需经过协调组件处理退役与补偿映射，见 [ADR-C02](#adr-c02)。 |

### H. 接收与命令名额

<a id="adr-h01"></a>

#### ADR-H01｜Flash command 先按长度、对齐和页边界检查，再拆为保留父 ID 的 64B fragments。

| 字段 | 记录 |
|---|---|
| 规范关系 | 长度/对齐/边界：直接规定；拆片和父ID保留：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | normalize_command()中的合法性约束与CommandFragment内部表示分别记录。将command拆成64B片段是组装实现方式，不是规范必须采用的软件数据结构；规范化辅助函数的异常接口与路径响应接口分别说明。 |
| 规范依据/无直接依据说明 | §4.1 p15：写 burst 为 4KiB 且 4KiB 对齐；读 burst 为 64B–4KiB、64B 对齐、不跨 4KiB；§5.3.1 p56 与 §5.4.1 p58。 |
| 关联要求/条目 | REQ-ADDR、REQ-ASSEMBLY |
| 选择理由 | 不是任意添加的“只支持完整页写 burst”限制；规范已有直接依据。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [normalize_command()](../src/runtime/command_admission.cpp)；[CommandFragment::parent/key/slot](../include/hbf_eventlab/runtime/command_admission.hpp) |
| 验证测试与覆盖 | [T02](verification.md#t02) 覆盖 4KiB 写拆分、读跨页、禁用 burst；[T12](verification.md#t12) 覆盖整条写 burst overlap。未穷举所有长度。 |
| 实现状态/验证状态 | 部分实现；V2 |
| 已知限制 | 本条只描述一个 channel-local command 的规范化；normalize_command() 的异常属于辅助接口诊断。64B 读的接受、发射、完成与状态响应由 [ReadPath](../src/runtime/read_path.cpp) 实现，见 ADR-D09；读 burst 尚未接入运行路径。 |

<a id="adr-h02"></a>

#### ADR-H02｜一个写 command 不论拆成多少片段只返回一次响应；成功需等 Core 数据提交。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定 |
| 条目性质 | 行为要求 |
| 分类边界说明 | 每command一次响应及Core提交后才成功都可以直接对应代码；父请求记录的具体容器不改变这条行为要求的性质。 |
| 规范依据/无直接依据说明 | §5.4.1(1)(2) p58，non-posted、每 command 一响应、数据写入 Core 后才成功。 |
| 关联要求/条目 | REQ-RESPONSE、[ADR-W03](#adr-w03) |
| 选择理由 | 规范直接要求；不能在组装收齐时就返回成功。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [accept() 父 ID 去重](../src/runtime/write_assembly.cpp)；[commit_program() 先 sink 后 retire](../src/runtime/write_assembly.cpp)；[WritePath::complete() 防双完成](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) 验证 burst 单响应和提交后释放；[T14](verification.md#t14)/[T17](verification.md#t17) 验证失败请求不被复活。 |
| 实现状态/验证状态 | 已实现于写路径；V1 |
| 已知限制 | 并未实现 AXI beat/ID 顺序与协议握手；completion 仅为抽象响应记录。 |

<a id="adr-h03"></a>

#### ADR-H03｜MOCS 合法选择为 1 或 256..16384 的 2 次幂，selection 不超过 capability。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定 |
| 条目性质 | 行为要求 |
| 分类边界说明 | 数量枚举和selection/capability约束直接对应profile验证。内部使用解码后数量并不需要另立一个分类条目。 |
| 规范依据/无直接依据说明 | §5.9.2.1 p70、§5.9.2.3 p71–72 的 MOCS 编码。 |
| 关联要求/条目 | REQ-MOCS |
| 选择理由 | 规范直接要求；内部保存解码后的数量而不是寄存器编码。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [legal_mocs()](../src/architecture/device_description.cpp)；[StaticDeviceDescription capability/selection 检查](../src/architecture/device_description.cpp) |
| 验证测试与覆盖 | [T02](verification.md#t02) 验证 MOCS=1 的部分组合；没有完整遍历全部合法/保留编码的专项测试。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | CommandAdmission 的独立组件测试可直接传入 limit=2；不能把该构造接口等同 OCP 配置验证器。 |

<a id="adr-h04"></a>

#### ADR-H04｜MOCS 满时不接受请求、不产生 completion、不创建介质工作；可选严格诊断模式。

| 字段 | 记录 |
|---|---|
| 规范关系 | MOCS满后的处理：规范未明确边界下的项目解释；严格诊断：无直接规定 |
| 条目性质 | 行为要求（项目解释）；可配置策略 |
| 分类边界说明 | 缺少专用错误码不单独证明必须背压。未接受、不产生completion是已选接口语义；严格诊断是独立组件可选方式，WritePath当前未提供切换入口。 |
| 规范依据/无直接依据说明 | MOCS 数量有规范依据；Table 13 p61–62 没有 MOCS-full 专用码。MOCS 满时采用未接受背压是项目解释，见 ADR-H04 的适用条件。 |
| 关联要求/条目 | REQ-MOCS、[ADR-H05](#adr-h05) |
| 选择理由 | 将未接受请求与已接受后的错误 completion 区分；不借用 Pending DLU 错误码。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [CommandAdmission::try_accept()](../src/runtime/command_admission.cpp)；[WritePath::submit() credit 预检查](../src/runtime/write_path.cpp)；[AdmissionMode](../include/hbf_eventlab/runtime/command_admission.hpp) |
| 验证测试与覆盖 | [T02](verification.md#t02) 验证 backpressure/strict mode；[T16](verification.md#t16) 验证不新增 erase/context，响应交付后恢复。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 不是 AXI 信号级背压实现；WritePath 固定使用默认 Backpressure，严格模式只在组件接口可选。 |

<a id="adr-h05"></a>

#### ADR-H05｜command credit 按 channel 与父 command 计数，并保留到响应交付。

| 字段 | 记录 |
|---|---|
| 规范关系 | command计数单位：直接规定；账本和抽象交付边界：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制；实现保障 |
| 分类边界说明 | MOCS按command而非fragment计数有直接语义依据；父ID账本和response_delivered()定义当前模型的名额释放边界，不能等同已实现AXI响应握手。 |
| 规范依据/无直接依据说明 | MOCS 为 outstanding commands；§5.4.1(1) 一 command 一响应。交付点由抽象 API 定义。 |
| 关联要求/条目 | REQ-MOCS、REQ-RESPONSE |
| 选择理由 | 防止把 fragment 数当作 command 数，或介质完成但响应尚未交付时提前归还名额。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [CommandAdmission::accepted_](../include/hbf_eventlab/runtime/command_admission.hpp)；[accept_parent() / Parent 记录](../src/runtime/write_path.cpp)；[response_delivered()](../src/runtime/read_path.cpp) |
| 验证测试与覆盖 | [T02](verification.md#t02)/[T03](verification.md#t03)/[T16](verification.md#t16) 覆盖 channel 隔离、重复交付拒绝、单 burst credit 和交付边界；`shared_credit`、`media_lifetime` 覆盖读写共享与读响应交付。 |
| 实现状态/验证状态 | 读写共享账本已接入；V1 |
| 已知限制 | 当前 ReadPath/WritePath 共用一个 CommandAdmission，构造时检查依赖一致；独立创建多个 ledger 不会自动共享预算。RX 驻留不计入 MOCS，见 ADR-LOOP-L02。 |

<a id="adr-h06"></a>

#### ADR-H06｜Pending DLU 满只拒绝打开新 DLU，已有 DLU 的未收片段仍可接收。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定 |
| 条目性质 | 行为要求 |
| 分类边界说明 | Pending DLU满时拒绝新DLU但允许已有DLU补齐是要求本身；计数器与查表代码就是落实该要求的位置。MOCS仍是独立接收条件。 |
| 规范依据/无直接依据说明 | Table 13 p61、§11.5.2.4 p121–122、§5.4.1(7) p58。 |
| 关联要求/条目 | REQ-PENDING、[ADR-H04](#adr-h04) |
| 选择理由 | 规范直接要求，允许 Host 优先补齐已有数据以释放资源。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [check_command() 按 existing context 区分](../src/runtime/write_assembly.cpp)；[accept() counts_ / limit_](../src/runtime/write_assembly.cpp)；[submit() 提交前检查 Pending 结果](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) 和 [T16](verification.md#t16) 覆盖新 DLU 0x4、补片成功、被拒绝起点不 erase。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 补片仍须有 MOCS credit；Pending 满与 MOCS 满不能互相替代。 |

<a id="adr-h07"></a>

#### ADR-H07｜MOCS先校验Supported／Selected合法性；64B写前进性按实际workload提示，不禁止超载实验。

| 字段 | 记录 |
|---|---|
| 规范关系 | MOCS数值范围与Supported／Selected关系有规范依据；Host工作模式诊断无直接规定 |
| 条目性质 | 行为要求；模型／输入假设；实现保障 |
| 分类边界说明 | 配置是否合法与某个workload能否完成分开。设备支持burst不表示Host使用burst，也不能据此拒绝纯读配置。 |
| 规范依据／无直接依据说明 | §5.9.2.1及选定MOCS相关配置语义；规范不要求模拟器拒绝合法但可能缺乏前进性的实验。 |
| 关联要求／条目 | REQ-MOCS、REQ-ASSEMBLY；[ADR-H05](#adr-h05)；[ADR-CFG-01](#adr-cfg-01) |
| 选择理由 | 保留纯读、完整burst及有意研究超时的配置，让资源不足经真实组装／响应路径表现。 |
| 模型假设／参数依据 | Supported和Selected各为1或256—16384内的2的幂，Selected≤Supported。实际输入含写且Host为fragment64、Selected<64时只警告。当前64B写需要64条未完成命令才能收齐一页；更大的MOCS也不保证多个DLU能同时补齐。 |
| 实现位置（函数／成员） | [StaticDeviceDescription构造函数](../src/architecture/device_description.cpp)校验数值；[diagnose_workload()](../src/configuration/simulation_configuration.cpp)只产生提示；[CLI](../examples/minimal_loop.cpp)在load后输出提示并执行。 |
| 验证测试与覆盖 | [configuration.cpp](../tests/configuration.cpp)：mocs_ranges、small_read、small_write、small_burst；[check_configuration_cli.cmake](../tests/check_configuration_cli.cmake)：read_small、write_small、burst_small检查退出语义及诊断。 |
| 实现状态／验证状态 | 已实现；V1 |
| 已知限制 | 不求解全局前进性；credit分散、工作顺序和有限缓冲仍可能导致超时或失败。 |

<a id="adr-h08"></a>

#### ADR-H08｜读写路径共用按 channel 计数的父 command 账本，credit 保留到各自响应交付。

| 字段 | 记录 |
|---|---|
| 规范关系 | 计数与响应约束：直接规定；共享账本：受规范约束，机制未指定；对象身份与禁复制检查：无直接规定 |
| 条目性质 | 行为要求；实现机制；实现保障 |
| 分类边界说明 | 本条扩展 H05 的写路径范围。共享同一个 CommandAdmission 是落实读写合计 outstanding 的方式；同地址依赖检查、禁复制属于宿主对象一致性保障，不是规范指定的类设计。 |
| 规范依据/无直接依据说明 | REQ-MOCS、REQ-RESPONSE 约束 command 计数及完成；规范未指定账本对象、C++ 引用身份或析构方式。 |
| 关联要求/条目 | REQ-MOCS、REQ-RESPONSE、[ADR-H05](#adr-h05)、[ADR-D10](#adr-d10) |
| 选择理由 | 避免读写各持一份预算而分别放行；错误响应和成功响应都维持父请求的完整生命周期。 |
| 模型假设/参数依据 | 单模拟线程；同一设备的 ReadPath 与 WritePath 必须引用同一 description/layout/coordinator/media/admission 实例；channel 间独立计数。 |
| 实现位置（成员/函数） | [CommandAdmission 接口与禁复制](../include/hbf_eventlab/runtime/command_admission.hpp)；[ReadPath 构造依赖检查](../src/runtime/read_path.cpp)；[try_accept()/response_delivered()](../src/runtime/command_admission.cpp) |
| 验证测试与覆盖 | [T22](verification.md#t22)、[T24](verification.md#t24)、[T35](verification.md#t35)：跨路径填满 credit、错误交付后释放、channel 隔离、重复 ID、错误账本与析构清理。 |
| 实现状态/验证状态 | 已接入读写路径；V1 |
| 已知限制 | 账本不是 AXI 同 ID 排序器；析构清理是模拟器关闭行为，不产生正常协议 completion。 |

<a id="adr-h09"></a>

#### ADR-H09｜路径内先处理 MOCS；公开事务入口先处理能力诊断，再进入建模语义裁决。

> 适用边界：“MOCS 先”只适用于已进入模型协议语义的请求。无效 channel/operation、payload shape、unsupported size 等 API 能力诊断可以在 RX/MOCS 之前直接报错；路径内的建模错误仍按 MOCS 后的固定顺序处理。

| 字段 | 记录 |
|---|---|
| 规范关系 | MOCS 拒收及多错误共现裁决：规范未明确边界下的项目解释；检查组织：受选定解释约束，机制未指定 |
| 条目性质 | 行为要求（项目解释）；实现机制 |
| 分类边界说明 | 优先级适用于能够路由到路径且不复用在途内部 CommandId 的请求。错误 op、channel 或重复内部 ID 仍属 API 诊断；本条不声称所有异常检查都排在 MOCS 后。 |
| 规范依据/无直接依据说明 | REQ-MOCS 与 Table12/13 给出能力、错误类型及响应范围，未给出本实现这张完整的共现优先级表；延续 H04 的未接受即不产生 completion 解释。 |
| 关联要求/条目 | REQ-MOCS、REQ-RESPONSE、[ADR-H04](#adr-h04)、[ADR-H10](#adr-h10)、[ADR-D05](#adr-d05)、[ADR-D06](#adr-d06) |
| 选择理由 | 满 credit 时不创建错误 completion，也不抢占尚未接收命令的内部资源；用显式入口次序保证可重复的裁决。 |
| 模型假设/参数依据 | MOCS → 可解释命令所需字段 → 地址/逻辑退役。写随后处理当前 DLU、恢复/顺序与新 DLU 名额，再申请修复/映射资源；读随后处理当前内容的 Pending、恢复限制、当前内容有效性、物理目标及 erase/quiesce 门禁。未建模错误不在此表内。 |
| 实现位置（成员/函数） | [WritePath::submit()](../src/runtime/write_path.cpp)；[ReadPath::submit()](../src/runtime/read_path.cpp) |
| 验证测试与覆盖 | [T21](verification.md#t21)、[T22](verification.md#t22)、[T23](verification.md#t23)、[T25](verification.md#t25)、[T32](verification.md#t32)、[T34](verification.md#t34) 覆盖直接调用 ReadPath/WritePath 时 credit 满先于建模错误与输入形状检查、字段先于地址、Pending 先于恢复、顺序先于新 DLU 名额。DeviceDriver 的公开事务入口另有 RX/MOCS 前的能力诊断，见 ADR-LOOP-L02。 |
| 实现状态/验证状态 | 已实现当前子集；V2（未涵盖全部规范错误） |
| 已知限制 | try_accept() 的严格诊断模式仍供独立调用；集成路径先用 has_credit() 返回 Backpressured。事件先后另见 E07，不能由入口优先级取代。 |

<a id="adr-h10"></a>

#### ADR-H10｜地址越界与已建模 User 字段错误转换为 0x1/0x3；不支持的模拟器输入保留 API 诊断。

| 字段 | 记录 |
|---|---|
| 规范关系 | 0x1/0x3 含义：直接规定；inspect_command 组织：受规范约束，机制未指定；支持范围及 API 异常边界：无直接规定 |
| 条目性质 | 行为要求；实现机制；模型／输入假设；实现保障 |
| 分类边界说明 | inspect_command() 返回可响应的错误，normalize_command() 保留为抛异常的诊断适配器。保留后者不表示集成读写路径仍在 MOCS 前用它拒绝 Host 请求。 |
| 规范依据/无直接依据说明 | Table12/13、§11.5.1.1/3、§11.5.2.1/3。当前只建模 HostCommand 暴露的 read_hint 等字段，不能据此宣称完整校验 AXI USER。 |
| 关联要求/条目 | REQ-ADDR、REQ-RESPONSE、[ADR-H01](#adr-h01)、[ADR-H09](#adr-h09) |
| 选择理由 | 把 Host 可见错误与调用方传入无法表示的模拟器请求分开；无效地址不伪造 LogicalBlockKey，也不创建 DLU。 |
| 模型假设/参数依据 | 0 长度、非 64B 倍数、未对齐、跨 DLU、未支持 burst、错误 token 数等继续诊断；ReadPath 当前只接 64B。字段/地址同时错时按当前顺序返回已建模错误，不保证优先诊断所有 API 形状。 |
| 实现位置（成员/函数） | [inspect_command()](../src/runtime/command_admission.cpp)；[normalize_command()](../src/runtime/command_admission.cpp)；[写错误转换](../src/runtime/write_path.cpp)；[读错误转换与长度边界](../src/runtime/read_path.cpp) |
| 验证测试与覆盖 | [T21](verification.md#t21)、[T22](verification.md#t22)：地址端点与 uint64 边界、非法 hint、输入诊断、满 credit 时不进入这些检查。 |
| 实现状态/验证状态 | 已实现明确的输入子集；V2 |
| 已知限制 | 不能把所有 throw 描述为规范错误响应；未来 AXI 适配层需另定未建模字段及传输形状的处理。 |

### W. 写入组装和顺序

<a id="adr-w01"></a>

#### ADR-W01｜组装 context 以 64 位 received bitmap 判断重复与完整性。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | bitmap是识别重复与完整性的一种具体机制；overlap及收齐后下发的行为要求分别由相关条目承载，规范未指定bitmap。 |
| 规范依据/无直接依据说明 | §5.4.1 p58、Table 13 overlap/timeout；规范要求行为，未指定 bitmap 数据结构。 |
| 关联要求/条目 | REQ-ASSEMBLY、REQ-OVERLAP |
| 选择理由 | 64 个 slot 精确覆盖一个 DLU，位图可直接检查重复片段和完整性。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [PendingWriteContext::received/phase/data_slot/awaiting_completion](../include/hbf_eventlab/runtime/write_assembly.hpp)；[accept() 重复检查与 received.all()](../src/runtime/write_assembly.cpp) |
| 验证测试与覆盖 | [T03](verification.md#t03)/[T12](verification.md#t12) 验证补片、重复、收齐及原数据不被覆盖。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 不支持 byte-enable/64B 内部分写；bitmap 本身不是规范要求的硬件组织。 |

<a id="adr-w02"></a>

#### ADR-W02｜整条 command 在修改任何片段前检查 overlap，拒绝时不部分提交。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 整command预检是实现拒绝重复command而不部分修改现有数据的机制。表13的错误作用域不自动规定必须先建立mask或采用当前事务边界。 |
| 规范依据/无直接依据说明 | 服务于 Table 13 对当前重复命令的拒绝语义；“先建立 mask”没有直接规范要求。 |
| 关联要求/条目 | REQ-OVERLAP、[ADR-H01](#adr-h01) |
| 选择理由 | 避免 burst 中间 slot 冲突之前已经写入前面 slot。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [WriteAssembly::check_command() 局部 mask](../src/runtime/write_assembly.cpp)；[submit() preflight 及后续 accept 循环](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T12](verification.md#t12) 检查 slot7 已存在时整个 4KiB 拒绝，received.count 仍为1。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 底层 accept(fragment) 不自动提供 command 原子性；调用者必须使用集成路径或自行满足前置条件。 |

<a id="adr-w03"></a>

#### ADR-W03｜Assembly storage 与 context 元数据分离，正常路径保留整页数据到 program 终止。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 组装与转发要求约束数据可用性，但未唯一决定存储组织和释放时刻。retained-data是当前基线；接口可替换不等于WritePath已经支持配置切换。 |
| 规范依据/无直接依据说明 | §5.4.1(3) 和 §5.4.3 要求组装及读转发；不规定物理缓冲组织和释放时机。 |
| 关联要求/条目 | REQ-ASSEMBLY、REQ-READFORWARD、REQ-RESPONSE |
| 选择理由 | 保留组装数据至操作终止，避免提前释放后缺少稳定的数据所有权和转发来源。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [IAssemblyStorage](../include/hbf_eventlab/interfaces/assembly_storage.hpp)；[RetainedAssemblyStorage::pages_](../include/hbf_eventlab/policies/retained_assembly_storage.hpp)；[retire()/commit_program()](../src/runtime/write_assembly.cpp)；[WritePath::storage_/assembly_](../include/hbf_eventlab/runtime/write_path.hpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) 检查 Programming 时仍可读数据、sink 异常不释放、成功提交先于释放。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | IAssemblyStorage 可替换，但 WritePath 成员硬编码 RetainedAssemblyStorage；不是当前可运行配置开关。容量至少覆盖所有 channel 的 Pending 总数。 |

<a id="adr-w04"></a>

#### ADR-W04｜重复片段、Ready/Programming 页及已成功写入的非起始页返回 overlap，原请求保持不变。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定；状态覆盖含直接推导 |
| 条目性质 | 行为要求 |
| 分类边界说明 | pending和已写NAND上的重复覆盖拒绝，以及只结束当前重复command，属于外部行为。Ready/Programming是代码中的适用状态表达；新page0例外边界另见ADR-W08。 |
| 规范依据/无直接依据说明 | Table 13 p61 明确仅当前重复 CMD；§11.5.2.2 p121 扩展到已经写入 NAND 的地址。 |
| 关联要求/条目 | REQ-OVERLAP、[ADR-W01](#adr-w01)、[ADR-W08](#adr-w08) |
| 选择理由 | 规范直接要求；Pending 表释放后仍通过顺序游标识别已写前缀。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [check_command() phase/mask](../src/runtime/write_assembly.cpp)；[submit() page < next_page_to_program()](../src/runtime/write_path.cpp)；[reject()](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T12](verification.md#t12) 覆盖 Collecting/Ready/Programming/已提交非零页；[T03](verification.md#t03) 原 token 不变。 |
| 实现状态/验证状态 | 已实现于当前写路径；V1 |
| 已知限制 | page-0 合法新起点的排空边界见 ADR-W08；已建模错误使用 ADR-H09 的固定检查顺序，未覆盖全部规范错误组合。 |

<a id="adr-w05"></a>

#### ADR-W05｜同一 block 顺序接收，不允许跳页；不同逻辑 block 各自维护状态。

| 字段 | 记录 |
|---|---|
| 规范关系 | 块内顺序/块间独立：直接规定，适用范围需结合上下文；分块状态：机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 以§11.5.2.6的具体块范围解释§5.4.1的一般表述：接收与 program 顺序在同一逻辑 block 内约束，具体边界见 [ADR-W06](#adr-w06)；不同 block 不共用一个顺序游标。块间能否同时执行还取决于资源，物理 Bank 基线见 [ADR-LOOP-R02](#adr-loop-r02)。按逻辑块持有状态是落实方式，状态容器不由规范指定。 |
| 规范依据/无直接依据说明 | §11.5.2.6 p122 允许不同 block 并行写而要求块内顺序；§5.4.1(4)(9) p58 的范围需结合该条理解。 |
| 关联要求/条目 | REQ-WRITEORDER |
| 选择理由 | 采用块内约束解释，不能把局部地址序列误写成整个 channel 只能一个全局递增游标。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [BlockWriteOrder::can_start()](../src/runtime/block_write_order.cpp)；[WritePath::blocks_ 以 LogicalBlockKey 分离](../include/hbf_eventlab/runtime/write_path.hpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) 直接检查 next-page admission；[T11](verification.md#t11)/[T19](verification.md#t19) 检查 remap 后旧游标不能沿用；不同块资源争用未测试。 |
| 实现状态/验证状态 | 已实现于功能模型；V2 |
| 已知限制 | 跨块资源争用由 DeviceDriver 的 Bank/UCIe 模型处理，Core Channel 当前为理想链路，见 ADR-LOOP-R01/R02/L04。写顺序条款的范围解释见本条规范依据。 |

<a id="adr-w06"></a>

#### ADR-W06｜前一页全部接收后才可开始下一页；program 则等待前一页 program 成功。

| 字段 | 记录 |
|---|---|
| 规范关系 | 接收边界：规范未明确边界下的项目解释；program依赖：受规范约束 |
| 条目性质 | 行为要求（项目解释）；实现机制 |
| 分类边界说明 | 将prior page sent具体化为收齐前一页，是选定解释；接收与program分别使用游标是内部机制。不能把该边界描述成规范逐字规定。 |
| 规范依据/无直接依据说明 | §11.5.2.6 p122 使用 prior page sent，未精确定义为组装完成还是 program 完成。 |
| 关联要求/条目 | REQ-WRITEORDER、[ADR-W05](#adr-w05) |
| 选择理由 | 该接收边界允许组装和介质执行流水化，同时不跨越实际 program 顺序。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [assembly_complete() 推进 next_assembly_](../src/runtime/block_write_order.cpp)；[can_program()/program_success() 与 next_program_](../src/runtime/block_write_order.cpp)；[collecting_/programming_/ready_](../include/hbf_eventlab/runtime/block_write_order.hpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) 验证两个游标、前页未收齐时拒绝下一页、前页 program 未成功时禁止下一页 program；[T14](verification.md#t14) 集成流水。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 同一 block 只有一个 Collecting 页和一个 Programming 页；不是 OCP 指定的数据结构。 |

<a id="adr-w07"></a>

#### ADR-W07｜合法 page0 第一个片段安排一次 auto-erase；erase 完成前不 program。

| 字段 | 记录 |
|---|---|
| 规范关系 | 起点auto-erase：直接规定；首个被接受片段触发及准备状态：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 规范要求从 page0/WL0/string0 重写时自动擦除；当前在合法 page0 首片被接受后登记 erase 候选，实际发射时才建立操作，并以准备状态阻止提前 program。允许 page0 中任意 64B slot 成为首片，是当前扁平 page 模型的输入边界，不是规范逐字规定；组装规则见 [ADR-W01](#adr-w01)，合法重写的排空条件见 [ADR-W08](#adr-w08)。 |
| 规范依据/无直接依据说明 | §5.4.1(5) p58、§11.5.2.2 p121 规定从 page0/WL0/string0 开始重写并自动擦除。 |
| 关联要求/条目 | REQ-RESTART、[ADR-W08](#adr-w08) |
| 选择理由 | 规范要求 auto-erase；用 erase 候选、发射后的操作 ticket 和准备状态区分接收、实际执行与完成。 |
| 模型假设/参数依据 | 扁平 page0 表示 block 写入起点；首个被接受的 64B 片段可以位于 slot 0–63 中任意位置，不要求先到 slot0。整页仍须收齐 64 个不同片段，且 erase 完成后才可 program；补片不重复触发 erase。 |
| 实现位置（成员/函数） | [submit() BeginBlock 登记 WaitingErase 并置 ErasePending](../src/runtime/write_path.cpp#L145)；[issue_erase()/finish_erase()](../src/runtime/write_path.cpp#L188)；[probe_program()/issue_program() 检查准备状态](../src/runtime/write_path.cpp#L208) |
| 验证测试与覆盖 | [T12](verification.md#t12) 覆盖第一片 slot7、补片不再 erase、erase 前不能 program。 |
| 实现状态/验证状态 | 已实现于扁平 page 模型；V2 |
| 已知限制 | 没有 WL/string 独立坐标或 erase-fail；page0 与物理 WL0/string0 的对应由扁平 page 抽象表达。任意 slot 可作首片的假设见本条模型边界，不能据此宣称规范明确允许所有页内到达顺序。 |

<a id="adr-w08"></a>

#### ADR-W08｜新 page0 等待旧写、erase、响应、逻辑读依赖与实际物理 lease 排空；不要求旧 block 写满。

| 字段 | 记录 |
|---|---|
| 规范关系 | 规范未明确边界下的项目解释 |
| 条目性质 | 行为要求（项目解释）；实现保障 |
| 分类边界说明 | 规范允许起点重写，但没有列举当前所有排空条件。响应、erase、已接受读的逻辑内容依赖及实际物理 lease 排空构成所选保守接收边界；设备不猜测 Host 未表达的新写入/replay 意图。 |
| 规范依据/无直接依据说明 | §11.5.2.2 允许起点重写；未逐项规定旧依赖排空及普通新内容与 replay 的识别边界。 |
| 关联要求/条目 | REQ-RESTART、REQ-OVERLAP |
| 选择理由 | 设备不能判断 Host 未表达的意图；不采用独立 block rewrite epoch。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [submit() RetryPage0/BeginBlock 分类与排空检查](../src/runtime/write_path.cpp)；[in_flight(actual physical)](../src/runtime/capacity_retirement_coordinator.cpp)；[Block::preparation/outstanding](../include/hbf_eventlab/runtime/write_path.hpp) |
| 验证测试与覆盖 | [T12](verification.md#t12) 验证未交付响应、读 lease 与普通覆盖写；[T16](verification.md#t16) 验证旧 collecting 的 backpressure 不阻挡补片。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 路径返回 Backpressured 时不建立新父命令或 DLU；已 Taken 的请求可驻留有限 Device RX 并等待重试。Host/Device 仍须让旧 DLU 补片和既有响应前进，或由 timeout 终止组装；不是任意输入的无死锁保证。 |

<a id="adr-w09"></a>

#### ADR-W09｜组装 timeout 重试同一页，不自动扩大为 block replay；page0 复用已有 erase 准备。

| 字段 | 记录 |
|---|---|
| 规范关系 | 允许整DLU重发：直接规定；page0复用erase：规范未明确边界下的项目解释 |
| 条目性质 | 行为要求；行为要求（项目解释）；实现机制 |
| 分类边界说明 | 规范允许Host重发整个超时DLU，不要求无限自动重试；设备保留同目标的erase准备为项目解释。Host自动驱动见[ADR-LOOP-H23](#adr-loop-h23)，不能把设备允许重试等同Host已选定某种重试策略。 |
| 规范依据/无直接依据说明 | §11.5.2.5 p122 允许重发整个 DLU；没有完整规定 page0 timeout 与已下发 erase 的关系。 |
| 关联要求/条目 | REQ-TIMEOUT、REQ-RESTART |
| 选择理由 | 组装失败不撤销已经发生的物理擦除，也不制造第二次 erase。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [submit() RetryPage0 条件](../src/runtime/write_path.cpp)；[expire() 保留 preparation](../src/runtime/write_path.cpp)；[assembly_timeout() 不推进 cursor](../src/runtime/block_write_order.cpp) |
| 验证测试与覆盖 | [T13](verification.md#t13) 覆盖 erase 中/完成后 page0 重试、普通页重试、ticket 复用和 PEC。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 只有相同目标及同一内容代次才能复用准备；discard/remap 后会失效。 |

<a id="adr-w10"></a>

#### ADR-W10｜Pending write 已收目标片段可转发，未收目标片段不能回退到旧 NAND。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定 |
| 条目性质 | 行为要求 |
| 分类边界说明 | 已收片段转发与缺片段 0xA 是本条行为要求。当前通过 ReadPath 共享 credit 接入完成响应，正式接受侧的固定观察与结果空间预留见 ADR-D05、ADR-LOOP-L08；有序 Pending 组合仍属于未支持范围。 |
| 规范依据/无直接依据说明 | §5.4.3 p59 明确区分已收片段与 pending 部分，后者读状态为 0xA。 |
| 关联要求/条目 | REQ-READFORWARD |
| 选择理由 | 读路径查询与写组装共用的覆盖视图，确保成功转发对应当前已收片段，缺片段返回 0xA；完整 Pending 接收边界见 ADR-D05、ADR-LOOP-L08。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [WriteAssembly::forward()](../src/runtime/write_assembly.cpp)；[WritePath::forward()](../include/hbf_eventlab/runtime/write_path.hpp)；[ReadPath::observe / immediate](../src/runtime/read_path.cpp#L35)。 |
| 验证测试与覆盖 | [T03](verification.md#t03)、[T12](verification.md#t12) 验证 Available/MissingFragment 及重复写不改变转发内容；`pending_forward`、`deadline_forward` 与 [Device：pending_frozen](../CMakeLists.txt#L130)、[Device：pending_recheck](../CMakeLists.txt#L132) 验证读响应及固定观察。 |
| 实现状态/验证状态 | 无序 64B Pending 读响应已接入；V2（有序组合未支持） |
| 已知限制 | 有序 Pending 读仍报 LIMIT-READ-001；read burst 未支持。无序 64B 转发与缺片段 0xA 已实现，不能据此宣称覆盖完整 §5.4.3。 |

<a id="adr-w11"></a>

#### ADR-W11｜入口拒绝只完成当前 command；已存在 DLU 的 program failure 按关联集合结束请求。

| 字段 | 记录 |
|---|---|
| 规范关系 | DLU 故障响应范围：直接规定；未形成 DLU 时的拒绝边界：规范未明确边界下的项目解释；reject 组织：受选定边界约束，机制未指定 |
| 条目性质 | 行为要求；行为要求（项目解释）；实现机制 |
| 分类边界说明 | reject() 记录当前父 command 与错误响应，不建立 DLU、不占 assembly storage、不取得介质 lease。已可解析逻辑块的拒绝可计入该块 outstanding，响应交付前仍会影响合法 page0 重启门禁。 |
| 规范依据/无直接依据说明 | Table13、§11.5.2.5–8 约束 DLU 及关联请求的错误范围；“尚无上下文的非法新请求只结束自身”是当前接收边界解释，不能替代已接收 DLU 的故障广播。 |
| 关联要求/条目 | REQ-WRITEORDER、REQ-PENDING、REQ-RESPONSE、[ADR-R01](#adr-r01)、[ADR-C04](#adr-c04) |
| 选择理由 | page0 未收齐时每个提前到达的 page1 分别报 0x6/info1，保留 page0；program fail 时失败页报 0x7，同块已接收后续 DLU 报 0x6/info2。 |
| 模型假设/参数依据 | 新起点修复耗尽并首次退役时尚未建立该 DLU，故当次 0x8/info1 只关联当前 command；未来异步退役必须遍历全部受影响且未最终完成的请求。 |
| 实现位置（成员/函数） | [reject() 与 ERR-ASYNC-RETIRE 批注](../src/runtime/write_path.cpp)；[新 DLU 名额与修复检查](../src/runtime/write_path.cpp)；[complete()](../src/runtime/write_path.cpp)；[finish_program()](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T30](verification.md#t30)、[T34](verification.md#t34)：重复提前页没有“失败 DLU”、已有 page0 继续等待并独立 timeout、失败与后续 DLU fanout；[T28](verification.md#t28)：首次 0x8 与后续 0x1。 |
| 实现状态/验证状态 | 入口拒绝与现有 program-failure 广播已实现；V1；异步容量退役未实现 |
| 已知限制 | 不把尚未到达的命令预归入失败集合；已生成最终响应不因后续事件重写；未实现 bank/die/channel 范围的异步故障传播。 |

### R. Replay与故障

<a id="adr-r01"></a>

#### ADR-R01｜Program fail 终止失败 DLU，并让同块已接受后续 DLU 返回 0x6/info2。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定 |
| 条目性质 | 行为要求 |
| 分类边界说明 | 失败DLU及同块后续DLU的规定响应范围直接对应代码。收集Ready/Collecting后继只是覆盖该范围的方法，不能把规则扩大到同bank所有block。 |
| 规范依据/无直接依据说明 | Table 13 p62、§11.5.2.6–7 p122，要求失败页 0x7 及后续页顺序错误。 |
| 关联要求/条目 | REQ-RESPONSE、REQ-REPLAY |
| 选择理由 | 规范直接要求；收集 Ready 与 Collecting 后继，不能留着等恢复后自动执行。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [program_failure() 汇集 following_pages](../src/runtime/block_write_order.cpp)；[finish_program() 错误传播](../src/runtime/write_path.cpp)；[fail_program()/fail_prior_program()](../src/runtime/write_assembly.cpp) |
| 验证测试与覆盖 | [T14](verification.md#t14)/[T17](verification.md#t17) 验证 Ready+Collecting 后继全部完成及 pending 清空；[T03](verification.md#t03) 检查后继集合。 |
| 实现状态/验证状态 | 已实现于当前写路径；V1 |
| 已知限制 | 不是整个 bank 的所有 block 一起失败；广域故障未实现。未来支持其他触发路径需复用 DLU 范围结束语义。 |

<a id="adr-r02"></a>

#### ADR-R02｜故障恢复终点采用失败页；重放从 block 起点开始，成功到达终点后放行新页。

| 字段 | 记录 |
|---|---|
| 规范关系 | 起点重放：直接规定；恢复终点：章节差异下的项目解释 |
| 条目性质 | 行为要求；行为要求（项目解释） |
| 分类边界说明 | 从起点恢复有规范依据；选择§11.5.2.7的失败页作为终点而不采用§5.7完整块序列，是已确认解释。不能自动推广到普通refresh范围。 |
| 规范依据/无直接依据说明 | §11.5.2.7 p122 明确写到 failed page；§5.7 p63 同时列出到 R3-1 的完整 block 序列。 |
| 关联要求/条目 | REQ-REPLAY |
| 选择理由 | 以 §11.5.2.7 对失败页的具体描述确定 program-fail 恢复终点；与其他章节的范围差异见本条规范依据。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [ReplayState::program_failed() / required_through_](../include/hbf_eventlab/runtime/replay_state.hpp)；[submit() 阻止越过恢复终点](../src/runtime/write_path.cpp)；[program_succeeded()](../include/hbf_eventlab/runtime/replay_state.hpp) |
| 验证测试与覆盖 | [T14](verification.md#t14) 检查失败页2→重放0..2→Host 重发3；[T17](verification.md#t17) 检查更远合并终点。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 普通 refresh 的范围不能自动等同故障前缀；完整 Host refresh 调度尚不存在。 |

<a id="adr-r03"></a>

#### ADR-R03｜重复 program failure 以 max(旧未完成终点, 新失败页) 合并一份恢复义务。

| 字段 | 记录 |
|---|---|
| 规范关系 | 规范未明确边界下的项目解释 |
| 条目性质 | 行为要求（项目解释）；实现机制 |
| 分类边界说明 | 重复故障保留旧未完成恢复范围是项目选择，max endpoint是实现该选择的机制。§5.7通知合并不能直接证明设备端这一算法。 |
| 规范依据/无直接依据说明 | §5.7 p63 提到重复 refresh 通知及 refresh 写失败例外；§11.5.2.7 描述恢复。两处均未直接规定设备维护 max endpoint 的算法。 |
| 关联要求/条目 | REQ-REPLAY、[ADR-R02](#adr-r02) |
| 选择理由 | 防止较早页再失败导致更远的旧恢复义务被截短。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [program_failed(): required_through_/last_failure_/failure_count_](../include/hbf_eventlab/runtime/replay_state.hpp)；[program_failure() 调用 replay_](../src/runtime/block_write_order.cpp)；[BlockWriteOrder::replay_](../include/hbf_eventlab/runtime/block_write_order.hpp) |
| 验证测试与覆盖 | [T17](verification.md#t17) 覆盖更早页、同终点、page0 再失败；[T18](verification.md#t18) 覆盖连续备用块替换。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 合并的是恢复范围，不是 Host 通知、请求数据或响应；不能声称实现了完整 §5.7 Host refresh 去重。 |

<a id="adr-r04"></a>

#### ADR-R04｜Timeout、再次 page0、换 spare 不解除恢复义务；通过恢复完成义务须顺序 program 成功至合并终点。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受选定恢复解释约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 该状态生命周期依赖ADR-R02/R03，而非新增独立规范要求。到合并终点成功是通过恢复完成义务的条件；显式放弃旧内容及永久退役属于另外的终结路径，不表示恢复成功。 |
| 规范依据/无直接依据说明 | 满足 ADR-R02/R03 选定的恢复解释；规范没有指定 ReplayState 内部状态机。 |
| 关联要求/条目 | [ADR-R02](#adr-r02)、[ADR-R03](#adr-r03)、[ADR-W09](#adr-w09)、[ADR-R08](#adr-r08) |
| 选择理由 | 机制保证恢复义务不能靠更换目标或重启流程消失。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [restart()/bind()/program_succeeded()](../include/hbf_eventlab/runtime/replay_state.hpp)；[restart_from_page0()](../src/runtime/block_write_order.cpp)；[bind_target()](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T14](verification.md#t14) 重复起点；[T17](verification.md#t17) timeout/旧 ticket/终点成功；[T18](verification.md#t18) 换 spare 不缩短义务。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 清除入口还包括显式 discard/remap 和永久退役，分别是放弃旧内容与终结不可用状态，不代表恢复成功。 |

<a id="adr-r05"></a>

#### ADR-R05｜失败响应逐次结束旧请求；Host 用普通逻辑 Write 重发数据，不引入 Replay opcode 或自动复制。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定；不复活旧请求为关联响应语义的直接推导 |
| 条目性质 | 行为要求 |
| 分类边界说明 | Host 重新计算逻辑地址并重新发写请求是恢复责任；当前 HostDriver/RecoveryManager 已实现有限 workload 的自动反馈恢复。设备没有独立 Replay opcode，内部仍维护恢复状态；Host 自动恢复的机制与限制见 ADR-LOOP-H11～H17。 |
| 规范依据/无直接依据说明 | §11.5.2.7 p122 要求 Host 重新计算地址并重写；§5.2.4.6 p48、§11.4 p117 禁止设备 remap 搬移用户活数据。 |
| 关联要求/条目 | REQ-REPLAY、REQ-ZONE、REQ-RESPONSE |
| 选择理由 | 遵守 Host 承担数据恢复的职责，不将旧错误请求静默复活。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [finish_program() 终止旧请求](../src/runtime/write_path.cpp)；[submit() 仍接收 HostCommand Write](../src/runtime/write_path.cpp)；[HostDriver::receive / make_replay](../src/host/host_driver.cpp#L304)；[WriteRetentionStore](../include/hbf_eventlab/host/retention_store.hpp#L15)。 |
| 验证测试与覆盖 | [T14](verification.md#t14)/[T17](verification.md#t17) 检查旧后继没有复活；[T18](verification.md#t18) 检查替换后仍需 Host 数据；[Host：recovery_once](../CMakeLists.txt#L120)、[Host：missing_retention](../CMakeLists.txt#L120)、[Host：persistent_failure](../CMakeLists.txt#L120) 覆盖自动恢复、数据缺失和恢复预算耗尽。 |
| 实现状态/验证状态 | 设备语义及自动 Host replay 已接入；V2（有限 workload 子集） |
| 已知限制 | Host 使用有限 WriteRetentionStore 保留原内容，缺数据或预算耗尽时明确终止相关逻辑工作；没有从 NAND 补齐恢复数据、完整 refresh 通知表或自动 refresh 调度。 |

<a id="adr-r06"></a>

#### ADR-R06｜故障输入区分 FailedReusable 与 FailedUnusable，不能从 Host-visible 0x7 推导永久损坏。

| 字段 | 记录 |
|---|---|
| 规范关系 | 两种错误语义有直接依据；故障输入划分受规范约束但机制未指定 |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | program fail与不可恢复容量是不同外部结果；FailedReusable/FailedUnusable是本模型的外部故障输入契约，不是规范规定的诊断算法或失效率模型。 |
| 规范依据/无直接依据说明 | Table 13 分别定义 program fail 与不可恢复容量；未指定二者之间的故障诊断算法。 |
| 关联要求/条目 | REQ-REPLAY、REQ-CAPACITY |
| 选择理由 | 区分可重用失败和物理不可用：前者保留目标，后者标记物理块等待修复。 |
| 模型假设/参数依据 | 人工故障注入；发生概率和物理判据未设定。 |
| 实现位置（成员/函数） | [ProgramOutcome](../include/hbf_eventlab/runtime/write_path.hpp)；[finish_program() 仅 FailedUnusable 标记坏块](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T14](verification.md#t14) reusable 不换映射；[T15](verification.md#t15)/[T18](verification.md#t18) unusable 换 spare 或退役。 |
| 实现状态/验证状态 | 已实现为外部输入；V1 |
| 已知限制 | 无概率、PEC 阈值或模拟电路故障模型；不能用这组枚举推导真实失效率。 |

<a id="adr-r07"></a>

#### ADR-R07｜健康目标的新内容/refresh 重写只擦除并重写原物理块，不自动触发 remap。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 健康目标原地重写是已选基线；规范未要求每次refresh换块，也不能由此反推规范禁止所有其他合法替换方案。 |
| 规范依据/无直接依据说明 | §5.4.1(5) auto-erase 与 §5.7 refresh；无条款要求每次 refresh 更换物理块。 |
| 关联要求/条目 | REQ-RESTART、[ADR-R06](#adr-r06) |
| 选择理由 | 将重写与坏块替换分开，避免仅因重写就改变映射。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [repair_block() 健康目标返回 Ready](../src/runtime/capacity_retirement_coordinator.cpp)；[BeginBlock 调 repair_block() 后 pin 原目标](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T12](verification.md#t12) 普通新内容同目标；[T14](verification.md#t14) reusable replay 同目标；[T15](verification.md#t15) 比较物理 PEC。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 没有独立 refresh 命令身份；是否是 refresh 由 Host 工作负载语义决定。 |

<a id="adr-r08"></a>

#### ADR-R08｜显式 discard + remap 成功后可放弃旧内容及 replay 义务，永久逻辑退役不可撤销。

| 字段 | 记录 |
|---|---|
| 规范关系 | 旧数据失效/逻辑退役约束有直接依据；清除replay义务为项目解释 |
| 条目性质 | 行为要求；行为要求（项目解释）；实现机制 |
| 分类边界说明 | Host放弃旧数据且remap成功后可清除旧恢复义务，是项目生命周期解释。永久逻辑退役仍保留；这一选择不新增标准discard命令。 |
| 规范依据/无直接依据说明 | §11.4.1 p117 要求 remap 前数据无效；“允许清除旧 replay debt”是当前内部生命周期解释，不是新增标准命令。 |
| 关联要求/条目 | REQ-ZONE、REQ-CAPACITY、[ADR-C10](#adr-c10) |
| 选择理由 | 现有文档明确选择；无须为 Host 明确放弃的内容继续恢复，但需区分内容失效与容量退役。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [submit() 按 contents 变化清理旧绑定](../src/runtime/write_path.cpp)；[discard_contents()/retire()](../src/runtime/block_write_order.cpp)；[ReplayState::discard_contents()/retire()](../include/hbf_eventlab/runtime/replay_state.hpp) |
| 验证测试与覆盖 | [T19](verification.md#t19) 检查 ABA 后旧 replay/准备失效；[T18](verification.md#t18) 检查 Retired 终态；[T20](verification.md#t20) 逻辑坏块不复活。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 延迟到下一次 submit 才更新 WritePath 状态；观察 order() 可能仍见旧代次。Host discard API 不是现有 CSR 实现，见 ADR-Z04/Z07 的当前模型边界。 |

### Z. Zone remapping

<a id="adr-z01"></a>

#### ADR-Z01｜每个channel划分等大zone，当前配置要求zone恰好覆盖整个channel。

| 字段 | 记录 |
|---|---|
| 规范关系 | 等大zone：直接规定；无尾部残区地覆盖channel：模型选择 |
| 条目性质 | 行为要求；模型／输入假设 |
| 分类边界说明 | “zone恰好覆盖整个channel”指多个等大zone整除并共同覆盖channel，不是每个zone都等于整个channel。identity初始化及无尾区检查不能一起标成规范直接规定。 |
| 规范依据/无直接依据说明 | §11.4.1 p117 等大 zone；初始 identity 表是项目实现选择。 |
| 关联要求/条目 | REQ-ZONE |
| 选择理由 | 等大zone来自§11.4.1；整除检查使当前数组式映射没有尾部残区。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [ZoneMapping::构造函数 state_->channels/zones](../src/runtime/zone_mapping.cpp)；[zone 大小整除 channel 容量](../src/architecture/device_description.cpp) |
| 验证测试与覆盖 | [T01](verification.md#t01) 覆盖 identity、后续 swap 和 channel 隔离；无独立重载持久化表。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | zone大小运行期间不可修改；额外block-group对齐见ADR-Z02。 |

<a id="adr-z02"></a>

#### ADR-Z02｜zone size 额外要求为完整 block group 整数倍，保持 remap 后 die/bank/page。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，但增加了模型限制 |
| 条目性质 | 模型／输入假设；实现保障 |
| 分类边界说明 | 完整block group对齐强于规范的block-size倍数条件。它确保当前布局不改变lane/page，但也排除了部分配置；这不是新增规范要求。 |
| 规范依据/无直接依据说明 | §11.4.1 p118 只要求 NAND block size 的倍数；当前限制更强，不能标为 Spec-Mandated。 |
| 关联要求/条目 | REQ-ZONE、[ADR-A04](#adr-a04) |
| 选择理由 | 代码注释明确 BASELINE stronger alignment；确保当前扁平布局和两游标在 remap 后语义一致。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [zone_size_dlus % block_group_dlus()](../src/architecture/device_description.cpp)；[commit_swap() lane/page-preserving 检查](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T01](verification.md#t01) 拒绝只一个 block 大小的 zone，并遍历检查 remap 后 page/lane。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 排除了部分规范仅按 block-size 对齐可表达的配置；自定义 layout 仍须满足该额外契约。 |

<a id="adr-z03"></a>

#### ADR-Z03｜zone swap 交换当前目标，不复制、擦除或搬移用户数据。

| 字段 | 记录 |
|---|---|
| 规范关系 | 直接规定 |
| 条目性质 | 行为要求 |
| 分类边界说明 | swap当前目标且不搬移活数据是直接行为要求。带hole补偿的扩展已单列为容量利用率研究策略，见ADR-C05/C06；本条不为扩展的规范等价性背书。 |
| 规范依据/无直接依据说明 | §5.2.4.6 p48 Remap Type=Swap；§11.4 p117 不支持 GC/活数据搬移。 |
| 关联要求/条目 | REQ-ZONE |
| 选择理由 | 规范直接要求；多次交换必须形成当前 permutation，不能硬写原始 zone ID。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [commit_swap_impl() std::swap(x.target,y.target)](../src/runtime/zone_mapping.cpp)；[translate() 保持 zone 内偏移](../src/runtime/zone_mapping.cpp) |
| 验证测试与覆盖 | [T01](verification.md#t01) 连续不同 pair 交换与注入性；[T05](verification.md#t05) 反向交换；[T10](verification.md#t10) 第三方数据不变；[T11](verification.md#t11) 磨损不迁移。 |
| 实现状态/验证状态 | 基础 swap 已实现；带补偿语义见 ADR-C06；V2 |
| 已知限制 | 仅单 pair、同 channel 的内部 API；未实现 opcode0x08、WDATA 多 pair 和 WLS 模式检查。 |

<a id="adr-z04"></a>

#### ADR-Z04｜Host 显式声明数据可丢弃；交换要求两 zone 均满足该声明，新 pin 消耗声明。

| 字段 | 记录 |
|---|---|
| 规范关系 | Host保证数据无效：直接规定；声明API和消费标志：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制；模型／输入假设 |
| 分类边界说明 | declare_discardable()是模拟Host承诺的输入接口，新pin消耗声明是保持该承诺新鲜度的机制；规范未要求这种布尔数据结构。 |
| 规范依据/无直接依据说明 | §11.4.1 p117 要求 Host 保证旧数据无效、重写前不再读；规范未定义 declare_discardable() API 或消费标志算法。 |
| 关联要求/条目 | REQ-ZONE |
| 选择理由 | 不从 NAND programmed/valid 位推断 Host 语义生命周期；布尔声明是当前模拟接口。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [declare_discardable_impl()](../src/runtime/zone_mapping.cpp)；[pin_impl() 清 discardable](../src/runtime/zone_mapping.cpp)；[swap_status() NotDiscardable](../src/runtime/zone_mapping.cpp) |
| 验证测试与覆盖 | [T01](verification.md#t01) 检查新 IO 后声明失效；[T05](verification.md#t05)–[T10](verification.md#t10) 的 remap 均经 prepare() 声明。 |
| 实现状态/验证状态 | 已实现 API 门槛；V2 |
| 已知限制 | 设备信任 Host 的数据可丢弃声明，无法验证其真实保留需求。提交 remap 后，当前内容代未建立时的读按 ADR-D12 返回防御性 0x7；这不构成对任意 Host 行为的完整协议验证。 |

<a id="adr-z05"></a>

#### ADR-Z05｜Zone swap 只阻止新映射 lease，并让可安全排空的旧工作继续。

> 适用边界：quiesce 不等于冻结所有已到达事务；已有 DLU 补片可以继续。Pending 转发/0xA 只有在无需新 lease 且通过自身前置能力检查时才可继续。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | quiesce门禁作用于新映射pin/lease申请。已有DLU补片复用旧binding；Pending读的转发或缺片段响应无需新lease，可在各自前置检查通过后完成。冻结不表示所有新到达命令均无法处理；仍需取得新lease的介质读和新DLU受门禁限制。旧lease排空保护在途地址。 |
| 规范依据/无直接依据说明 | REQ-ZONE约束remap前后数据与访问边界；本机制以阻止新lease并排空旧lease保护在途地址，规范未指定quiesce/lease协议。Pending读的内容与错误处理见D05，其接入后的排空边界见D11。 |
| 关联要求/条目 | REQ-ZONE、[ADR-W08](#adr-w08)、[ADR-W03](#adr-w03)、[ADR-D05](#adr-d05)、[ADR-D11](#adr-d11) |
| 选择理由 | 防止旧请求使用已更换映射；同时避免冻结导致旧组装永远无法收齐。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [quiesce_pair_impl()](../src/runtime/zone_mapping.cpp)；[pin_impl() 与 in_flight](../src/runtime/zone_mapping.cpp)；[submit() 复用已有 binding](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T01](verification.md#t01)/[T11](verification.md#t11) 检查新lease门禁和旧lease排空；[T16](verification.md#t16) 已有context仍能补片；[T19](verification.md#t19) timeout后erase lease阻止remap。[T32](verification.md#t32)覆盖quiesce下Pending转发、缺片段响应与已有DLU补片。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 需要调用者处理取消或重试，不提供通用 wait queue；单模拟线程。 |

<a id="adr-z06"></a>

#### ADR-Z06｜缓存失效回调完成后才提交 remap；异常保留旧映射及 quiesce。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 同步失效回调与失败保留旧状态是内部提交契约；规范没有指定 C++ 回调。当前 DeviceDriver 注册 BankCache 全标签失效，已完成读的受保护结果仍可交付；这只覆盖当前缓存模型。 |
| 规范依据/无直接依据说明 | 无直接规定同步 C++ 回调的条款；维护 remap 后数据/地址一致性。 |
| 关联要求/条目 | REQ-ZONE、[ADR-C09](#adr-c09) |
| 选择理由 | 将失效确认设为提交边界，避免先换映射再失败。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [commit_swap_impl() invalidate() 后更新 permutation](../src/runtime/zone_mapping.cpp)；[commit_swap() 统一提交回调](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T01](verification.md#t01)/[T07](verification.md#t07) 检查异常不提交、回调看到旧视图、重入被拒绝；[Device：remap_cache](../CMakeLists.txt#L131)、[Device：cache_pin_invalidation](../CMakeLists.txt#L131) 覆盖集成失效与受保护结果。 |
| 实现状态/验证状态 | 事务框架与 Device Cache 失效已接入；V2 |
| 已知限制 | 当前 DeviceDriver 已注册 Cache invalidator；remap/repair 采用保守的全 Cache 标签失效，受保护结果独立保留。外部回调的副作用不会由映射事务自动回滚，必须满足自身异常契约。 |

<a id="adr-z07"></a>

#### ADR-Z07｜Zone Remapping 当前直接使用内部 API，默认工作在 Host-controlled WL 场景。

| 字段 | 记录 |
|---|---|
| 规范关系 | Host-controlled模式条件：直接规定；直接API及默认场景：模型选择 |
| 条目性质 | 行为要求；模型／输入假设 |
| 分类边界说明 | WLS条件是规范要求，当前以输入场景假设满足而未实现协议门禁。明确区分“假设调用合法”与“代码检查该条件”。 |
| 规范依据/无直接依据说明 | §5.2.4.6 p48 规定 BUCC.WLS=10b 才能执行 opcode0x08；§11.4 p117 区分 Host/自动 WL。 |
| 关联要求/条目 | REQ-ZONE、REQ-SCOPE |
| 选择理由 | 当前不实现 CSR/ADMIN，调用满足模式条件属于输入前提，未由协议门禁检查。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [declare_discardable()/quiesce_pair()/commit_swap()](../include/hbf_eventlab/runtime/capacity_retirement_coordinator.hpp)；[DeviceOperatingConfiguration 不含 WLS](../include/hbf_eventlab/architecture/device_description.hpp) |
| 验证测试与覆盖 | [T01](verification.md#t01)/[T05](verification.md#t05) 等验证内部 swap；无 WLS、opcode、非法 ZoneID 协议响应测试。 |
| 实现状态/验证状态 | 语义子集；V2 |
| 已知限制 | 不具备完整 Zone Remapping command 一致性；多 pair、字段错误及协议完成语义待实现。 |

### C. 容量退役与补偿

**补偿策略的声明边界：** 稀疏补偿是本项目选择的容量利用率研究策略，并非 OCP 指定的坏块处理算法。若没有补偿，坏物理块可能使所在逻辑 zone 的可用容量受到大于单块的影响；若允许带永久逻辑退役地址的 zone 交换而不补偿，接收坏物理块的一方可能新增一个退役地址，原有退役地址又不能恢复，退役范围会随交换扩大。现有策略以低密度块级例外尝试保留健康物理块的利用率，密度上限见 ADR-C07；它仍须满足逻辑退役不可复活、映射无别名及 remap 不搬移数据等外部约束。策略分类不等于证明带例外映射与规范的 zone swap 完全等价。

**Host 可见性边界：** 补偿策略不改变规范层面的逻辑地址可用性、退役容量信息、命令结果及 zone PEC 等 Host 可见信息；Host 不直接观察补偿表、最终物理块映射或逐物理块 PEC 归属，内部测试可以检查这些实现细节。这使是否启用补偿不成为 Host 工作负载格式或恢复逻辑的前提。§11.4.1 要求支持 Host 查询 zone PEC；本模拟器尚未接入该 CSR，将来接入时须定义补偿后的 zone PEC 统计口径，不能把 zone PEC 一并隐藏。

<a id="adr-c01"></a>

#### ADR-C01｜映射、退役、替换和 IO pin 的状态权威与统一协调入口。

| 字段 | 记录 |
|---|---|
| 规范关系 | 状态管理与统一变更入口：受规范约束，机制未指定；具体C++防绕过与身份检查：无直接规定 |
| 条目性质 | 实现机制（状态所有权与统一变更入口）；实现保障（防绕过与身份检查） |
| 分类边界说明 | 本条关注权威状态由谁持有、谁有权协调修改。各类状态可由各自负责的组件保存，跨组件变更统一经Coordinator；协调器不需要另存一套同类事实。候选计划和派生视图不构成第二套已提交的权威状态。单一入口用于约束修改权，一次修改如何一致生效及失败时如何处理见C09。 |
| 规范依据/无直接依据说明 | REQ-ZONE、REQ-CAPACITY约束映射变更和已退役地址处理；明确状态归属及受控入口，用于防止相关状态被各调用方独立改写。规范未指定协调器类、组件划分或C++ managed_/身份检查。补偿映射的研究策略和规范等价性边界见 C05/C06。 |
| 关联要求/条目 | REQ-ZONE、REQ-CAPACITY、[ADR-C09](#adr-c09)、[ADR-C05](#adr-c05)、[ADR-C06](#adr-c06) |
| 选择理由 | 明确已提交状态的权威来源和跨组件修改权限，防止绕过一致性检查或形成第二套同类事实；事务的提交与失败保障由C09另行记录。 |
| 模型假设/参数依据 | 权威状态可分布在各自负责的组件中，由Coordinator统一协调变更；候选计划仅在成功提交后成为已提交状态。本条无独立数值参数。 |
| 实现位置（成员/函数） | [构造/析构与 require_idle()](../src/runtime/capacity_retirement_coordinator.cpp)；[require_unmanaged()](../src/runtime/zone_mapping.cpp)；[pin()/replace_failed() 的 managed_ 门禁](../src/runtime/block_replacement.cpp) |
| 验证测试与覆盖 | [T07](verification.md#t07) 拒绝底层 pin/修改、重复 coordinator 及回调重入。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 是受控 API 契约，不是所有 C++ 状态写入的安全边界；外部仍能直接持有 media。依赖生命周期需调用者保证。 |

<a id="adr-c02"></a>

#### ADR-C02｜最终解析先拒绝逻辑退役，再查 nominal slot 的 hole；未命中才走 replacement。

| 字段 | 记录 |
|---|---|
| 规范关系 | 退役地址拒绝：直接规定；hole/replacement优先级：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 无效逻辑容量的外部约束与三层解析结构分别说明；查表顺序是当前组合映射机制，且inspect/pin用途不同。 |
| 规范依据/无直接依据说明 | Table 12/13 后续退役地址无效；没有直接规定此三层查表结构。 |
| 关联要求/条目 | REQ-CAPACITY、[ADR-A05](#adr-a05) |
| 选择理由 | 统一实际目标，避免 Allocator/Device 等不同组件重复解码形成不一致。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [slot()/resolve_unchecked()/inspect()](../src/runtime/capacity_retirement_coordinator.cpp)；[AddressResolver::slot()/inspect()/pin()](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T05](verification.md#t05) 检查 retired 拒绝与 hole page/offset；[T11](verification.md#t11) 双映射；[T20](verification.md#t20) 实际写入最终补偿目标。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | inspect 仅用于观察，可能显示已知待修复目标；实际介质 IO 通过 pin 获取 lease。ReadPath 在物理解析前检查永久逻辑退役并返回读 0x1，见 ADR-D08；直接调用底层解析接口仍可能得到诊断异常。 |

<a id="adr-c03"></a>

#### ADR-C03｜坏块用同 channel/die/bank 单调分配的 spare 替换，跳过坏 spare，不复用退役块。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，分配机制未指定 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 坏块管理有规范依据；同lane、单调spare分配和跳过坏spare共同构成当前基线。算法目前硬编码，不因存在替代研究方案就标为已可配置策略。 |
| 规范依据/无直接依据说明 | §4.1 p15 内部坏块管理、§11.5.2.8 p123 spare 线索；分配算法未由规范指定。 |
| 关联要求/条目 | REQ-CAPACITY、[ADR-R06](#adr-r06) |
| 选择理由 | 采用容易实现、结果确定的默认基线：每个 channel/die/bank 维护单调游标，按 spare 编号依次选取尚未使用且健康的块，跳过坏块和退役块；替换保持 lane/page/offset 不变。该方式便于维护分配状态、复现故障处理及验证耗尽边界，不代表最优的磨损或可靠性策略。 |
| 模型假设/参数依据 | spare 数量可配置，选择算法当前固定。后续可在合法候选集合内比较其他策略；若 spare 的健康、磨损和访问成本完全相同，仅改变选取顺序通常只改变物理编号，不能据此主张性能或寿命收益。 |
| 实现位置（成员/函数） | [plan_replace(): next_spare/entries/retired](../src/runtime/block_replacement.cpp)；[repair_block() healthy 检查](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T11](verification.md#t11)/[T15](verification.md#t15)/[T18](verification.md#t18) 同 bank spare 和耗尽；[T07](verification.md#t07) 跳过坏 spare、回调失败不消耗。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 策略硬编码，未注入独立 PlacementPolicy；跨 lane 替换、spare 回收和坏块概率未实现。 |

<a id="adr-c04"></a>

#### ADR-C04｜无法替换时永久标记逻辑 block，首次写报告 0x8/info1，后续读写为 0x1。

| 字段 | 记录 |
|---|---|
| 规范关系 | 退役及后续错误：直接规定；修复耗尽的检测时机：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 外部0x8/info1及后续0x1有依据；当前在后续起点修复时检测spare耗尽，是具体路径选择。RAM退役集合不是跨电源周期持久化的实现。 |
| 规范依据/无直接依据说明 | Table 13 p62、§11.5.2.1 p121、§11.5.2.8 p123；§11.5.1.8 p120 要求 Host 维护不可用逻辑容量。 |
| 关联要求/条目 | REQ-CAPACITY、REQ-RESPONSE |
| 选择理由 | 规范要求外部结果；先失败0x7、后续起点修复耗尽才0x8是当前检测时机。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [repair_block() staged retired_](../src/runtime/capacity_retirement_coordinator.cpp)；[submit() 已退役返回0x1](../src/runtime/write_path.cpp)；[新退役返回0x8](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T15](verification.md#t15)/[T18](verification.md#t18)/[T20](verification.md#t20) 写路径；[T05](verification.md#t05)/[T08](verification.md#t08) 检查 swap 后永久退役与仅新增受影响 owner。 |
| 实现状态/验证状态 | 块级内存态已实现；V2 |
| 已知限制 | 未实现 bank/die/channel 广域退役、CSR 位图和跨电源周期持久化；当前发生0x8时未建立该新DLU，不能据此认定所有在途退役场景已覆盖。 |

<a id="adr-c05"></a>

#### ADR-C05｜swap 只重新分配双方实际拥有的健康物理块，保持第三方所有权及逻辑退役集合。

| 字段 | 记录 |
|---|---|
| 规范关系 | 正确性边界受规范约束；双边资源池是无直接算法规定的补偿研究策略 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 第三方所有权和永久退役不被破坏是正确性约束；仅以双方实际拥有资源组池是本项目的补偿方案。“受约束”不等于已证明补偿式交换与规范的 zone swap 完全等价。 |
| 规范依据/无直接依据说明 | 受 §5.2.4.6、§11.4.1 和退役地址不可复活的约束；资源池算法非规范直接要求。 |
| 关联要求/条目 | REQ-ZONE、REQ-CAPACITY |
| 选择理由 | 已有补偿时，物理几何 zone 内的块可能属于第三方，不能按物理位置借走。按实际所有权组池，旨在避免一次坏块使整个 zone 失去健康容量，或在交换中把逻辑退役传播给另一方。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [commit_swap(): Owner/owners/free 构造](../src/runtime/capacity_retirement_coordinator.cpp)；[validate_invariants()](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T09](verification.md#t09) 钉住第三方实际目标；[T10](verification.md#t10) 固定种子400次尝试，独立比较池集合、外部映射/数据及 retired 集合。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 测试仅覆盖所列功能不变量，不证明带例外的映射与规范的 zone swap 完全等价；策略适用边界仍需在发布说明中明示。 |

<a id="adr-c06"></a>

#### ADR-C06｜稀疏补偿条目直接保存最终物理块；匹配优先默认目标，再原目标，再同 lane 有序目标。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接算法规定；补偿研究策略的规范等价性尚未证明 |
| 条目性质 | 实现机制 |
| 分类边界说明 | RetiredHoleMap 及贪心匹配顺序是项目选定的补偿研究策略，匹配优先级目前固定；可配置的是 ADR-C07 的密度上限。分类只说明来源和用途，不证明带例外映射与规范 zone swap 完全等价。 |
| 规范依据/无直接依据说明 | 规范未定义 RetiredHoleMap 或该优先级；本机制在逻辑退役不可复活和 remap 不搬移数据等约束下，协调永久逻辑退役与 zone permutation。§11.4.1 的 zone PEC 查询要求不因隐藏逐块物理归属而消失。 |
| 关联要求/条目 | [ADR-C02](#adr-c02)、[ADR-C05](#adr-c05) |
| 选择理由 | 低密度例外旨在保留仍健康的物理容量，避免整 zone 容量受单块故障牵连或退役地址随交换扩大。匹配顺序当前固定为默认目标、原目标、同 lane 有序目标，旨在减少默认映射外的条目，但没有全局最优性证明。合法资源池与所有权约束见 [ADR-C05](#adr-c05)，可配置的补偿预算见 [ADR-C07](#adr-c07)；预算可配置不代表匹配优先级可替换。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [RetiredHoleMap::Entries/lookup()/commit()](../include/hbf_eventlab/runtime/retired_hole_map.hpp)；[commit_swap() assigned/free 与 holes](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T05](verification.md#t05) 单孔、反向交换删除例外；[T06](verification.md#t06) 对称孔；[T10](verification.md#t10) 连续交换与无别名。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 未证明最少补偿项；不提供可替换匹配器；不形成映射链。补偿表、最终物理映射及逐块 PEC 归属为设备内部信息；未来若暴露 zone PEC CSR，须明确补偿后的统计口径。 |

<a id="adr-c07"></a>

#### ADR-C07｜补偿条目预算默认50‰，分母为每 channel nominal blocks，向下取整且无最小一项。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 可配置策略；模型／输入假设 |
| 分类边界说明 | 密度预算提供配置参数，50‰、分母和向下取整是当前具体选择；没有测量或规范数值来源，也不是PEC差异阈值。配置在构造后冻结。 |
| 规范依据/无直接依据说明 | 无直接 OCP 数值依据；不能与 §11.4.1 p118 可选 PEC 差异阈值混为一谈。 |
| 关联要求/条目 | [ADR-C06](#adr-c06)、REQ-MODEL |
| 选择理由 | 限制块级例外元数据规模有文档依据；具体50‰来源/选择理由待确认，不推断为硬件测量结果。 |
| 模型假设/参数依据 | 默认50‰，测试还用100/250/1000‰；均未硬件校准。 |
| 实现位置（成员/函数） | [CapacityPolicy::max_hole_permille](../include/hbf_eventlab/architecture/device_description.hpp)；[hole_limit()](../src/runtime/capacity_retirement_coordinator.cpp)；[commit_swap() 密度检查](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T05](verification.md#t05) 默认20块预算1；[T06](verification.md#t06) 超限原子拒绝及小几何预算0；[T10](verification.md#t10) 使用250‰，不是默认5%压力证明。 |
| 实现状态/验证状态 | 参数已实现；V1 |
| 已知限制 | 运行时不可修改；低密度不等于低硬件成本；可能拒绝某些 zone pair。 |

<a id="adr-c08"></a>

#### ADR-C08｜DensityLimit/RecoveryRequired 等失败不部分提交，并保留 quiesce，调用方需取消或重试。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定；内部事务契约 |
| 条目性质 | 实现保障 |
| 分类边界说明 | 这些返回值和失败保留quiesce是项目API约定；一致提交和调用方重试责任不能冒充标准ADMIN错误语义。 |
| 规范依据/无直接依据说明 | 无直接规定这些内部返回码；属于事务与前进性契约。 |
| 关联要求/条目 | [ADR-C07](#adr-c07)、[ADR-Z05](#adr-z05) |
| 选择理由 | 避免失败后仍改变部分映射；不为满足密度而额外退役健康地址。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [commit_swap() RecoveryRequired/InFlight](../src/runtime/capacity_retirement_coordinator.cpp)；[DensityLimit](../src/runtime/capacity_retirement_coordinator.cpp)；[repair_block() 拒绝 quiesced 目标](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T06](verification.md#t06) 失败保留冻结；[T09](verification.md#t09) cancel→repair→重新swap；[T10](verification.md#t10) 随机失败快照不变。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 没有自动恢复调度器，盲目在冻结 pair 内反复 repair 不会前进；内部码未映射为 ADMIN 响应。 |

<a id="adr-c09"></a>

#### ADR-C09｜候选准备、同步失效和统一提交保证映射与退役状态一致。

| 字段 | 记录 |
|---|---|
| 规范关系 | 跨组件一致提交：受规范约束，机制未指定，并受选定映射解释限定；具体C++异常安全与容器提交手段：无直接规定 |
| 条目性质 | 实现保障（事务一致性与失败语义）；实现机制（分阶段提交） |
| 分类边界说明 | 本条关注一次变更怎样生效及提交前失败的结果。行为层面是在约定提交边界内使映射、hole、逻辑退役和spare分配一致生效；候选容器、C++异常处理与无异常交换是当前宿主实现手段。必须保证相应的一致提交与失败语义，三阶段流程是项目选用的机制，不能写成规范指定的唯一实现。 |
| 规范依据/无直接依据说明 | REQ-ZONE、REQ-CAPACITY关联的映射结果与退役地址处理约束本条的一致性目标；补偿部分仍依赖已选解释及其待确认边界。规范未要求C++候选容器、事务快照或map swap；这些具体宿主保障分别说明为无直接规定。 |
| 关联要求/条目 | REQ-ZONE、REQ-CAPACITY、[ADR-C01](#adr-c01)、[ADR-C04](#adr-c04)、[ADR-C05](#adr-c05)、[ADR-C06](#adr-c06)、[ADR-Z06](#adr-z06) |
| 选择理由 | 候选元数据先准备，提交前同步失效成功后再发布，避免异常消耗spare或只更新部分关系；提交前失败不发布候选元数据。已确认的物理损坏事实不随候选元数据放弃而回滚。 |
| 模型假设/参数依据 | 单模拟线程、同步失效回调；事务边界覆盖候选元数据，物理故障事实及回调外部副作用不在回滚承诺内。本条无独立数值参数。 |
| 实现位置（成员/函数） | [repair_block() staged plan/retired/holes](../src/runtime/capacity_retirement_coordinator.cpp)；[commit_swap() staged contents 与 no-throw commit](../src/runtime/capacity_retirement_coordinator.cpp)；[BlockReplacement::commit()](../include/hbf_eventlab/runtime/block_replacement.hpp) |
| 验证测试与覆盖 | [T07](verification.md#t07) 检查失效抛异常、重入、旧视图、不消耗spare；[T08](verification.md#t08) 补偿目标退役异常。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 单线程事务；不承诺所有宿主bad_alloc恢复或外部cache回调副作用回滚，也不提供多线程或断电事务原子性。 |

<a id="adr-c10"></a>

#### ADR-C10｜discard/remap 成功才推进逻辑 zone content generation，以识别 A→B→A 的内容失效。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受已选内容生命周期解释约束，机制未指定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | generation是防止旧内容状态在ABA后被误用的身份机制；不是PEC、独立block重写代次或规范字段。其推进条件依赖ADR-R08。 |
| 规范依据/无直接依据说明 | 规范无 generation 字段；实现 §11.4.1 数据已失效前提下的内部状态隔离。 |
| 关联要求/条目 | [ADR-R08](#adr-r08)、[ADR-Z04](#adr-z04) |
| 选择理由 | 仅比较最终物理坐标识别不了 ABA；普通 page0/spare 替换不能借此解除 replay。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [contents_](../include/hbf_eventlab/runtime/capacity_retirement_coordinator.hpp)；[commit_swap() 递增双方内容代次](../src/runtime/capacity_retirement_coordinator.cpp)；[submit() 清理旧 cursor/preparation/binding](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T19](verification.md#t19) ABA 旧失败前缀、旧erase准备与旧ticket；[T05](verification.md#t05) 双交换代次2。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 不是 block rewrite epoch，也不是 PEC；观察接口在下次submit前可能尚未清旧状态。 |

<a id="adr-c11"></a>

#### ADR-C11｜实际 IO 同时持有 zone、nominal slot 和最终物理目标 lease，替换快照共享计数。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 三层lease保护当前组合映射的实际在途目标；数量和共享计数实现不是规范规定，也不是已完成读路径的证据。 |
| 规范依据/无直接依据说明 | 无直接数据结构要求；保障在途地址不能被 remap/repair/新erase改变。 |
| 关联要求/条目 | [ADR-Z05](#adr-z05)、[ADR-C02](#adr-c02)、[ADR-W08](#adr-w08) |
| 选择理由 | hole target 可能不在 nominal zone 内；仅 pin slot 不够保护实际物理块。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [ResolvedAddressLease::zone_/replacement_/physical_lease_](../include/hbf_eventlab/runtime/address_resolver.hpp)；[pin()](../src/runtime/capacity_retirement_coordinator.cpp)；[ReplacementState::active 共享计数](../src/runtime/block_replacement.cpp)；[Binding/Parent/Erase 持有 shared lease](../include/hbf_eventlab/runtime/write_path.hpp) |
| 验证测试与覆盖 | [T07](verification.md#t07) 无关旧lease在替换快照后仍正确递减；[T08](verification.md#t08) 实际补偿块被读钉住；[T09](verification.md#t09)/[T19](verification.md#t19) 排空边界。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 读路径已集成逻辑与物理保护，finish 后稳定结果由独立有界存储持有，见 ADR-LOOP-P06/R07；外部仍须遵守依赖销毁顺序。 |

<a id="adr-c12"></a>

#### ADR-C12｜不变量检查器验证无物理别名、同lane、合法范围、密度、无孤立/冗余hole。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | 检查器验证本方案的不变量，其中部分还包含项目额外限制；不能将通过检查器等同通过全部OCP要求。 |
| 规范依据/无直接依据说明 | 无直接规范算法；用于检查当前映射机制的内部正确性。 |
| 关联要求/条目 | [ADR-C05](#adr-c05)、[ADR-C06](#adr-c06) |
| 选择理由 | 将映射一致性变为可执行诊断；require_healthy 可允许已知故障暂待修复。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [CapacityRetirementCoordinator::validate_invariants(bool)](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T05](verification.md#t05)–[T10](verification.md#t10)/[T18](verification.md#t18)/[T20](verification.md#t20) 多处调用；[T10](verification.md#t10) 另有独立所有权集合/第三方数据断言。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 检查器不是形式化证明；部分测试依赖它作为oracle。枚举整个几何的宿主运行成本不代表硬件成本。 |

### E. 事件与响应

<a id="adr-e01"></a>

#### ADR-E01｜同 Tick 按因果轮次推进，并在正常闭包后批量结算 deadline。

> 适用边界：同 Tick 执行顺序由 round、phase、ID 和 deadline 批结算共同决定，详见 ADR-LOOP-E01～E03。

| 字段 | 记录 |
|---|---|
| 规范关系 | 同刻deadline裁决：规范未明确边界下的项目解释；排序结构：无直接规定 |
| 条目性质 | 行为要求（项目解释）；实现保障 |
| 分类边界说明 | 截止 Tick 先推进所有正常事件及其因果轮次，再冻结并统一结算组装 deadline；这只判定当前 DLU 是否按期收齐，NAND program 与 Host 写成功仍由后续流程确定。该边界是项目解释；Tick、round、phase、ID 及 deadline 批结算共同提供确定性执行。 |
| 规范依据/无直接依据说明 | §5.4.1(6)、§11.5.2.5 规定时间限制，但未规定等时刻事件优先级。 |
| 关联要求/条目 | REQ-TIMEOUT |
| 选择理由 | 用同 Tick 因果轮次表达零延迟反馈，避免仅按单层 phase 排序截断有效补片链；正常闭包结束后统一结算同刻 deadline，避免超时反馈救回同批其他 DLU。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [EventQueue::schedule_at / run_next](../src/infrastructure/event_queue.cpp#L5)；[EventPhase / EventQueue](../include/hbf_eventlab/infrastructure/event_queue.hpp)。 |
| 验证测试与覆盖 | [T04](verification.md#t04)/[T13](verification.md#t13) 覆盖基础顺序和截止场景；现行轮次与批结算由 [生命周期：event_rounds](../CMakeLists.txt#L141)、[生命周期：deadline_credit_chain](../CMakeLists.txt#L146)、[生命周期：deadline_two_dlus](../CMakeLists.txt#L146)、[生命周期：deadline_batch](../CMakeLists.txt#L141) 验证。过去 Tick 被拒绝；同 Tick 已执行 phase 或更早 phase 的新事件进入下一轮。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 不能外部直接调用 submit 绕过事件阶段后，仍声称同Tick优先级成立；尚无广域故障phase约定。 |

<a id="adr-e02"></a>

#### ADR-E02｜首片建立固定deadline，后续补片不续期；只对Collecting超时并完成本DLU所有命令。

| 字段 | 记录 |
|---|---|
| 规范关系 | 起算/固定窗口/DLU错误范围：直接规定；Collecting门控：机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 首片起算、补片不续期和错误作用域由规范约束；用Collecting状态判断仍缺数据是实现方式，不把此组装timer扩展为program-timeout。 |
| 规范依据/无直接依据说明 | §5.4.1(6) p58 从首个64B起算；Table13 p61、§11.5.2.5 p122 规定DLU范围错误。 |
| 关联要求/条目 | REQ-TIMEOUT、REQ-RESPONSE |
| 选择理由 | 规范要求时间限制与范围；内部清理顺序由实现承担。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [accept() first_arrival/deadline](../src/runtime/write_assembly.cpp)；[expire()](../src/runtime/write_assembly.cpp)；[submit() Ready取消或安排deadline](../src/runtime/write_path.cpp)；[WritePath::expire()](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) 多父命令timeout、Ready不超时；[T13](verification.md#t13) page0/普通页重试；[T17](verification.md#t17) replay中timeout。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 数据释放和同页重试采用 ADR-W09 的解释；未实现 program-timeout。自动执行模型统一使用 1 Tick = 1 ps，默认时限见 ADR-LOOP-M01 和使用说明。 |

<a id="adr-e03"></a>

#### ADR-E03｜取消deadline并用context generation复核，旧事件不得终止新context；析构取消自有deadline。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | 取消、context generation及析构清理防止过期回调影响新context；这是对象与事件生命周期保障，不需要为它虚构OCP内部字段。 |
| 规范依据/无直接依据说明 | 无直接generation规范要求；属于回调/对象生命周期正确性。 |
| 关联要求/条目 | REQ-TIMEOUT、[ADR-E02](#adr-e02) |
| 选择理由 | 取消和身份检查分别防止无用执行及地址复用误命中；必要的是不变量而非特定计数器。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [PendingWriteContext::generation 分配](../src/runtime/write_assembly.cpp)；[expire() generation/phase 检查](../src/runtime/write_assembly.cpp)；[~WritePath()](../src/runtime/write_path.cpp)；[deadline lambda捕获generation](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) 旧timeout不能杀新context；[T13](verification.md#t13) 析构后无活动deadline；[T17](verification.md#t17) 相关恢复义务不受旧ticket影响。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 取消不撤销已发出的物理操作；外部harness安排的完成回调须自行遵守生命周期。 |

<a id="adr-e04"></a>

#### ADR-E04｜run_next先推进事件时间，再取出事件；invoke在回调前消费事件身份，异常不重复执行该事件。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | 正常执行路径先由 next_time() 排除队首已取消事件并确定下一时刻，run_next() 必要时更新 now_，再取出待执行事件；invoke() 在调用 callback 前移除 live_ 中的身份并更新执行状态与计数。因此 callback 抛异常后，该事件仍已消费；这些是模拟器执行契约，不是 OCP 规定的硬件行为。 |
| 规范依据/无直接依据说明 | 无直接OCP依据；离散事件模拟器执行契约。 |
| 关联要求/条目 | [ADR-E01](#adr-e01)、[ADR-LOOP-E02](#adr-loop-e02)、[ADR-LOOP-E03](#adr-loop-e03)、REQ-MODEL |
| 选择理由 | callback看到正确now并可安排后续事件；禁止递归执行runner，取消事件不推进时间。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [cancel()/prune()](../src/infrastructure/event_queue.cpp#L21)；[next_time()](../src/infrastructure/event_queue.cpp#L25)；[run_next() 时间推进、出队与截止批次](../src/infrastructure/event_queue.cpp#L50)；[invoke() live_/executing_/计数](../src/infrastructure/event_queue.cpp#L39) |
| 验证测试与覆盖 | [T04](verification.md#t04) 覆盖取消、时间不倒退、runner重入、异常消耗、异常后继续运行。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 一次 run_next() 可执行整个已冻结的 deadline 批次，不保证只调用一个 callback；截止批次规则见 [ADR-LOOP-E03](#adr-loop-e03)。不回滚 callback 已产生的外部状态修改。同 Tick 自调度由 callback/轮次限额中止，见 [ADR-LOOP-F03](#adr-loop-f03)；这些限额不构成任意程序的活锁证明。 |

<a id="adr-e05"></a>

#### ADR-E05｜erase/program 使用一次性OperationId，完成接口才更新media；未知或重复ticket拒绝。

| 字段 | 记录 |
|---|---|
| 规范关系 | Core完成前不得成功：直接规定；ticket和介质更新接口：受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 本条记录 OperationId 与完成接口，所服务的行为要求见 ADR-H02；DeviceDriver 负责资源占用和完成事件，路径负责 ticket 校验及介质状态更新。 |
| 规范依据/无直接依据说明 | §5.4.1要求成功在Core写入后；不规定模拟器ticket。 |
| 关联要求/条目 | REQ-RESPONSE、REQ-RESTART |
| 选择理由 | 操作身份与block内容/物理PEC分离，旧完成不能误操作新流程。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [WritePath::probe_program / issue_program / finish_program](../src/runtime/write_path.cpp)；[issue_erase / finish_erase](../src/runtime/write_path.cpp)；[erases_ / programs_](../include/hbf_eventlab/runtime/write_path.hpp)；[DeviceDriver::execute_media](../src/runtime/device_driver.cpp#L276) |
| 验证测试与覆盖 | [T12](verification.md#t12)/[T17](verification.md#t17) 旧program ticket；[T19](verification.md#t19) 旧erase ticket；其他写路径测试显式驱动完成。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 低层测试可显式调用完成接口；自动执行中由 DeviceDriver 根据 Bank 资源和配置时延安排完成事件。未实现 erase-fail 结果及其恢复路径。 |

<a id="adr-e06"></a>

#### ADR-E06｜错误响应、不接受请求和 API 诊断保持不同边界。

| 字段 | 记录 |
|---|---|
| 规范关系 | 错误码/作用域有直接依据；未接受背压边界见项目解释；协议抽象范围为模型选择 |
| 条目性质 | 行为要求（项目解释）；实现机制；模型／输入假设 |
| 分类边界说明 | Table12/13 给出相关响应要求；本实现仅支持已列明子集。建模错误在正式接受后产生协议响应，Backpressured/NotTaken 与 API 能力诊断分别处理。显式 read_group 已约束组内交付，但不等于完整 AXI 编码与响应适配。 |
| 规范依据/无直接依据说明 | Table12/13 p59–62定义完整错误；Table8 p42涉及响应与ID顺序。当前是抽象子集。 |
| 关联要求/条目 | REQ-RESPONSE、REQ-SCOPE |
| 选择理由 | 阶段性实现选择来自既有设计文档；不把诊断异常伪装成Host已收到正确错误码。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [WriteStatus / ReadStatus](../include/hbf_eventlab/common/types.hpp#L106)；[WritePath::submit / reject](../src/runtime/write_path.cpp)；[ReadPath::submit / response_delivered](../src/runtime/read_path.cpp#L70)；[DeviceDriver](../src/runtime/device_driver.cpp)。 |
| 验证测试与覆盖 | [T12](verification.md#t12)～[T20](verification.md#t20) 覆盖基础写响应；`basic_errors`、`shared_credit`、`ingress_scope`、`event_error_order` 覆盖建模错误及作用域；[Device：ordered_error](../CMakeLists.txt#L129)、[Device：ordered_late_error](../CMakeLists.txt#L133) 覆盖显式组内错误交付。未覆盖完整 AXI、Die blocked 或 ECC 警告。 |
| 实现状态/验证状态 | 部分实现；V2 |
| 已知限制 | 已建模地址越界和 User 错误产生 0x1/0x3；未支持的 API 形状等仍是诊断。旧 axi_id 不自动转成 read_group；读 0x4/0x5/0x6/0x8/0x9、Die 级传播等未实现。Table13 的 DLU 作用域需随异步退役等扩展继续检查，不能用当前单 command 拒绝概括全部 0x6/0x8。 |

<a id="adr-e07"></a>

#### ADR-E07｜集成错误裁决遵守既有事件阶段；先生成的最终响应不被后来故障改写。

| 字段 | 记录 |
|---|---|
| 规范关系 | 等时刻事件顺序：沿用规范未明确边界下的项目解释；一次最终响应与生命周期：受规范约束，机制未指定 |
| 条目性质 | 行为要求（项目解释）；实现保障 |
| 分类边界说明 | 新增的是读写错误竞争的集成验证，未新增全局 phase。Completion→Arrival→Deadline 仍是 E01 的项目规则；同一命令在一次入口检查中的错误先后是另一维度。 |
| 规范依据/无直接依据说明 | REQ-TIMEOUT、REQ-RESPONSE 规定超时及响应范围；规范未给出本模拟器的 Tick/phase 排序。 |
| 关联要求/条目 | REQ-TIMEOUT、REQ-RESPONSE、[ADR-E01](#adr-e01)、[ADR-H09](#adr-h09)、[ADR-W11](#adr-w11) |
| 选择理由 | 用事件序列决定当时有效状态，避免在响应已确定后追溯改变结果或重复完成。 |
| 模型假设/参数依据 | 同 Tick 的 program fail 先于 assembly deadline，可令后续 DLU 获得 0x6/info2；deadline 已在较早 Tick 生成 0x5 时，后来失败不改写它。Arrival 可先观察 Pending，再由该 Tick 的 Deadline 使写请求超时。 |
| 实现位置（成员/函数） | [EventPhase](../include/hbf_eventlab/infrastructure/event_queue.hpp)；[complete()](../src/runtime/write_path.cpp)；[event_error_order()](../tests/read_path.cpp)；[deadline_forward()](../tests/read_path.cpp) |
| 验证测试与覆盖 | [T30](verification.md#t30)、[T31](verification.md#t31)：同刻失败/超时、先超时后失败、恢复成功与读 Arrival 同刻、已转发读不被随后写 timeout 改写。 |
| 实现状态/验证状态 | 当前事件子集已集成验证；V1 |
| 已知限制 | 直接调用 submit 不自动获得事件阶段排序。尚无完整 Device 调度、广域 Die 故障或并发线程模型。 |

### M. 介质及模型假设

<a id="adr-m01"></a>

#### ADR-M01｜以64个DataToken表示一页内容；稀疏map保存功能介质，初始视为已擦除。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | DataToken、稀疏map和初始全擦除分别是数据抽象、存储方式、初态假设；它们不因代码实现需要而成为协议要求。 |
| 规范依据/无直接依据说明 | 无OCP要求使用token/map或初始全擦除；属于当前功能抽象和初始化假设。 |
| 关联要求/条目 | REQ-MODEL、[ADR-A01](#adr-a01) |
| 选择理由 | 代码明确为functional media；足以检查数据身份与覆盖，但不承担位级NAND仿真。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [PageData = array<DataToken,64>](../include/hbf_eventlab/interfaces/assembly_storage.hpp)；[NandMediaState::Block/pages/blocks_](../include/hbf_eventlab/runtime/nand_media_state.hpp)；[read() 未写页nullptr](../src/runtime/nand_media_state.cpp) |
| 验证测试与覆盖 | [T03](verification.md#t03) token数据转移；[T12](verification.md#t12)/T20实际program后内容；T10第三方数据不变。未见独立全擦除初始/所有非法media调用测试。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 宿主PageData大小不是4096B硬件容量；没有ECC、保持时间、坏位、部分program痕迹。 |

<a id="adr-m02"></a>

#### ADR-M02｜物理页只能按next_page顺序program；erase清页并重置物理进度，不清除磨损历史。

| 字段 | 记录 |
|---|---|
| 规范关系 | 顺序与擦除约束有直接依据；扁平物理进度和清理机制未指定 |
| 条目性质 | 实现机制；模型／输入假设；实现保障 |
| 分类边界说明 | next_page与稀疏页状态用于独立检查物理目标；扁平page模型不是对多bit/WL/string编程顺序的完整实现。不清磨损历史与ADR-M03的物理记账约定关联。 |
| 规范依据/无直接依据说明 | §5.4.1、§11.5.2.2/6约束顺序及erase；精确介质状态容器为本项目机制。 |
| 关联要求/条目 | REQ-WRITEORDER、REQ-RESTART |
| 选择理由 | 以独立物理不变量防止仅逻辑游标正确、实际目标却被重复program。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [NandMediaState::erase()/program()](../src/runtime/nand_media_state.cpp)；[Block::next_page/unusable/pages](../include/hbf_eventlab/runtime/nand_media_state.hpp) |
| 验证测试与覆盖 | [T12](verification.md#t12)/[T15](verification.md#t15)/T20覆盖集成重写/替换；未见直接针对所有非法物理页序列的专项测试。 |
| 实现状态/验证状态 | 已实现；V2 |
| 已知限制 | 不模拟多bit NAND/WL/string程序顺序、真实电压步骤或失败后部分数据改变。 |

<a id="adr-m03"></a>

#### ADR-M03｜PEC基线在成功erase时加一，磨损跟随物理块，remap/replay不转移或清零。

| 字段 | 记录 |
|---|---|
| 规范关系 | PEC用于磨损管理有依据；精确记账模型未指定 |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | 成功erase加一、磨损保留在物理块是当前REFERENCE记账模型。失败周期如何累计未实现，不能据此主张已校准真实寿命。 |
| 规范依据/无直接依据说明 | §11.4.1 p117依据PEC做WL；未完整规定模拟器如何计入失败擦写周期。 |
| 关联要求/条目 | REQ-MODEL、REQ-ZONE |
| 选择理由 | 现有文档明确REFERENCE记账；保留逐物理块磨损是WL建模所需。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [erase() b.erases++](../src/runtime/nand_media_state.cpp)；[erase_count()](../src/runtime/nand_media_state.cpp) |
| 验证测试与覆盖 | [T11](verification.md#t11) remap不转移/清零；[T13](verification.md#t13) timeout重试不增PEC；T15新旧spare分别计数；T05补偿不erase。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 未提供真实PEC/寿命校准；没有失败周期、初始磨损分布、阈值退役或zone平均PEC统计。 |

<a id="adr-m04"></a>

#### ADR-M04｜功能时延参数不代表校准后的性能或硬件成本。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设 |
| 分类边界说明 | Tick 为模拟时间单位，自动执行配置定义 1 Tick = 1 ps；固定 NAND/Cache 延迟和小型几何用于功能验证，不是实测产品参数。时间单位明确不等于时延已经校准。 |
| 规范依据/无直接依据说明 | 没有该示例数值的规范、测量或SSD校准依据。 |
| 关联要求/条目 | REQ-MODEL、REQ-SCOPE |
| 选择理由 | 采用确定的功能参数重现顺序、争用和反馈行为，同时将物理时延与硬件成本的校准状态独立说明。 |
| 模型假设/参数依据 | 自动执行的 Tick 单位、sense/program/erase、Cache、链路和容量参数见 [参数说明](parameters.md)及 ADR-LOOP-M01/M02。组件示例的手工完成间隔仅服务对应场景。 |
| 实现位置（成员/函数） | [ExecutionConfiguration](../include/hbf_eventlab/architecture/execution_configuration.hpp)；[DeviceDriver::execute_media](../src/runtime/device_driver.cpp#L276)；[Tick](../include/hbf_eventlab/common/types.hpp)；[组件示例](../examples/foundations_demo.cpp) |
| 验证测试与覆盖 | 资源、时延参数执行和 Host 反馈的功能覆盖见 [验证矩阵](verification.md#2-测试矩阵)。物理时延与硬件成本未校准；功能测试通过不改变该结论。 |
| 实现状态/验证状态 | 演示模型已实现；V0（参数校准未做） |
| 已知限制 | 已实现 Bank、UCIe 和有界缓冲的功能资源模型；Core Channel 采用理想链路。没有可靠器件时延、功耗或面积校准；C++ 容器耗时/内存不能直接解释为硬件开销。 |

### D. 读取和维护策略接口

<a id="adr-d01"></a>

#### ADR-D01｜Batch hint立即关闭当前读窗口：R R B B R → [R R B] [B]，末尾R等timer。

| 字段 | 记录 |
|---|---|
| 规范关系 | 规范歧义下的项目解释 |
| 条目性质 | 行为要求（项目解释） |
| 分类边界说明 | B立即关闭窗口是已选解释，窗口交付与NAND执行必须区分。不是把标准语义任意开放为可切换研究策略。 |
| 规范依据/无直接依据说明 | §5.3.2.2 p57正文“until regular read”与示例触发点、字段位置存在解释空间；本项目选择已明确。 |
| 关联要求/条目 | REQ-READDISPATCH |
| 选择理由 | 将 Batch 作为当前集合的结束边界，减少等待后续读到达的依赖；窗口释放调度资格，实际发射仍受资源与顺序约束。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [ReadDispatchWindow::arrive()](../src/policies/read_dispatch.cpp)；[ReadHint](../include/hbf_eventlab/common/types.hpp) |
| 验证测试与覆盖 | [T04](verification.md#t04) 检查 RRBB 及末尾 R timer；[Device：window_batch_timer](../CMakeLists.txt#L128)、[Device：batch_pending](../CMakeLists.txt#L128)、[Device：batch_missing](../CMakeLists.txt#L128) 覆盖集成窗口及 Batch 的 Pending 分支。 |
| 实现状态/验证状态 | 已集成 Device 读窗口；V2 |
| 已知限制 | 已由 DeviceDriver 接入 per-channel 读窗口、Cache 与 Bank/Sense 约束；窗口关闭只释放调度资格，不等于立即执行 NAND sense。 |

<a id="adr-d02"></a>

#### ADR-D02｜Regular read窗口策略支持固定timer或队列深度阈值，加timer兜底。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，调度策略未指定 |
| 条目性质 | 可配置策略 |
| 分类边界说明 | wait/threshold 是可选择参数与策略；DeviceDriver 已接入 per-channel 窗口及主动 timer，释放后的候选仍须满足 Bank/Cache、sense 顺序和组内返回约束。 |
| 规范依据/无直接依据说明 | §5.3.2.1 p57允许累积与优化调度；未规定具体wait/threshold数值或算法。 |
| 关联要求/条目 | REQ-READDISPATCH、REQ-MODEL |
| 选择理由 | 为读取组织研究提供受约束策略入口；timer防止只有少量请求时永远等不到阈值。 |
| 模型假设/参数依据 | 测试wait10/20 Tick、threshold2仅演示，无校准依据。 |
| 实现位置（成员/函数） | [IReadDispatchPolicy::ready()/deadline()](../include/hbf_eventlab/interfaces/read_dispatch.hpp)；[WindowDispatchPolicy::wait_/threshold_](../include/hbf_eventlab/policies/read_dispatch.hpp)；[flush_due()/next_wakeup()](../src/policies/read_dispatch.cpp) |
| 验证测试与覆盖 | [T04](verification.md#t04) 覆盖固定 timer、新窗口期限和 threshold2；[Device：window_batch_timer](../CMakeLists.txt#L128)、[Device：window_threshold](../CMakeLists.txt#L128)、[Device：window_channel_isolation](../CMakeLists.txt#L128) 覆盖集成窗口与通道隔离。 |
| 实现状态/验证状态 | 接口、策略及主动 timer 已集成；V2 |
| 已知限制 | ReadDispatchWindow 通过 next_wakeup 暴露期限，DeviceDriver::update_timer 负责注册与更新 EventQueue 事件。窗口只决定候选释放资格，Cache hit、Bank 和 sense 顺序由后续调度检查；wait/threshold 尚未经过物理校准。 |

<a id="adr-d03"></a>

#### ADR-D03｜WearLevelingPolicy只能建议交换pair，正确性由映射组件检查。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，策略未指定 |
| 条目性质 | 可配置策略（接口预留）；实现保障 |
| 分类边界说明 | 策略建议pair与映射组件检查合法性构成职责边界。接口预留研究策略位置，但没有已实现的WL算法或自动执行循环。 |
| 规范依据/无直接依据说明 | §11.4–11.4.1 p117允许Host/产品相关WL；未规定具体策略。 |
| 关联要求/条目 | REQ-ZONE、REQ-MODEL |
| 选择理由 | 接口注释明确策略观察physical PEC与Host discard信息，不由策略决定合法性。 |
| 模型假设/参数依据 | 本条无独立数值参数；见本条适用条件及限制。 |
| 实现位置（成员/函数） | [ZoneObservation](../include/hbf_eventlab/interfaces/wear_leveling.hpp)；[IWearLevelingPolicy::choose()](../include/hbf_eventlab/interfaces/wear_leveling.hpp) |
| 验证测试与覆盖 | 无具体实现与直接测试；remap机制测试不能当作WL策略验证。 |
| 实现状态/验证状态 | 仅接口；V0 |
| 已知限制 | 没有自动WL loop、Observation采集器、Hot/Cold选择算法或PEC成本模型。 |

<a id="adr-d04"></a>

#### ADR-D04｜最小 ReadPath 接入 64B 读及 0x0/0x1/0x2/0x3/0x7/0xA 响应，以 DataToken 表示数据。

| 字段 | 记录 |
|---|---|
| 规范关系 | 状态含义与基本读粒度：直接规定；接入方式：受规范约束，机制未指定；支持子集与 token 抽象：无直接规定 |
| 条目性质 | 行为要求；实现机制；模型／输入假设 |
| 分类边界说明 | Table12 的状态含义有直接依据；仅实现六类结果、仅接 64B 是当前范围选择。read_burst 能力标志不代表路径已实现 read burst；窗口、Cache 与 Bank 调度由集成的 DeviceDriver 负责。 |
| 规范依据/无直接依据说明 | Table12、§5.4.3、§11.5.1.1/2/3/7；当前错误响应没有有效 DataToken，成功响应才携带 token。 |
| 关联要求/条目 | REQ-RESPONSE、REQ-READFORWARD、REQ-SCOPE、[ADR-D01](#adr-d01)、[ADR-D02](#adr-d02) |
| 选择理由 | 以接收、Pending 转发、介质读完成和响应交付构成功能边界；当前已与 DeviceDriver/HostDriver 接通，便于独立验证错误语义及集成调度。 |
| 模型假设/参数依据 | ReadPath 保留 Regular/Batch hint；DeviceDriver 管理读窗口、Cache 与 Bank 调度。数据仍以 DataToken 表达，不保存真实 64B 位数据，不实现 ECC 解码。 |
| 实现位置（成员/函数） | [ReadSubmission/ReadResponse/ReadOperation 与范围说明](../include/hbf_eventlab/runtime/read_path.hpp)；[submit()](../src/runtime/read_path.cpp)；[finish_read()](../src/runtime/read_path.cpp) |
| 验证测试与覆盖 | [T21](verification.md#t21)、[T23](verification.md#t23)、[T24](verification.md#t24)：基本错误、转发、未写页、介质读与响应生命周期。 |
| 实现状态/验证状态 | 最小功能路径已实现；V2（相对于完整规范读路径） |
| 已知限制 | 读 0x4/0x5/0x6/0x8/0x9、CECC/UECC、广域故障和 read burst 未实现。显式 read_group 已支持普通读组内顺序；旧 axi_id 不自动映射为组，完整 AXI 编码及有序 Pending 组合未支持。 |

<a id="adr-d05"></a>

#### ADR-D05｜当前内容代的 Pending DLU 优先提供已收片段；目标片段缺失返回 0xA，不回退旧 NAND。

| 字段 | 记录 |
|---|---|
| 规范关系 | Pending 转发与缺片段结果：直接规定；与恢复限制组合的优先级：规范未明确边界下的项目解释；查询组织：受规范约束，机制未指定 |
| 条目性质 | 行为要求；行为要求（项目解释）；实现机制 |
| 分类边界说明 | W10 原来只记录组件能力，本条补充真实读响应及共享 credit 接入。Pending 分支先于当前选定的恢复读限制；这不消除前面的 MOCS、字段、地址及逻辑退役检查。 |
| 规范依据/无直接依据说明 | §5.4.3 与 Table12；恢复限制是 D06 的产品策略，本条不将二者组合优先级冒称为规范完整错误优先级。 |
| 关联要求/条目 | REQ-READFORWARD、REQ-ASSEMBLY、[ADR-W10](#adr-w10)、[ADR-D06](#adr-d06)、[ADR-D07](#adr-d07) |
| 选择理由 | 防止读到正在被覆盖的旧 NAND 数据；直接复用 assembly 的 received bitmap 和保留数据。 |
| 模型假设/参数依据 | 覆盖 Collecting、Ready、Programming 期间仍保留的 context；只查询当前 content generation。成功转发和缺片段错误均是已接受命令，需要交付响应后释放 credit。 |
| 实现位置（成员/函数） | [当前内容检查与 forward 分支](../src/runtime/read_path.cpp)；[WriteAssembly::forward() 所在组件](../src/runtime/write_assembly.cpp) |
| 验证测试与覆盖 | [T23](verification.md#t23)、[T25](verification.md#t25)、[T31](verification.md#t31)、[T32](verification.md#t32)：已收/未收片段、Ready/Programming、replay 期间 Pending 优先、deadline 及 quiesce 场景。 |
| 实现状态/验证状态 | 已接入最小读路径；V1 |
| 已知限制 | 当前仅单个 64B 目标；未来读 burst 不能直接假定整条请求可由一个片段结果代表。 |

<a id="adr-d06"></a>

#### ADR-D06｜当前内容处于恢复阶段时，Pending处理之后的其余读返回0x2；合并终点program成功后放行。

| 字段 | 记录 |
|---|---|
| 规范关系 | 0x2 类型及允许产品定义限制原因/持续条件：直接规定；本项目选用的恢复限制策略：受规范约束，机制未指定，并依赖既定 replay 解释 |
| 条目性质 | 行为要求；实现机制（固定策略） |
| 分类边界说明 | §11.5.1.2 允许产品定义限制范围、原因和解除条件；它没有要求“replay 一律报 0x2”。AwaitingRestart/Replaying 到合并终点的限制是本项目当前选择，尚无可替换策略接口，不标为可配置策略。 |
| 规范依据/无直接依据说明 | Table12、§11.5.1.2；合并终点和恢复义务生命周期依赖 R02–R04 的项目解释。 |
| 关联要求/条目 | REQ-REPLAY、[ADR-R02](#adr-r02)、[ADR-R03](#adr-r03)、[ADR-R04](#adr-r04)、[ADR-D05](#adr-d05)、[ADR-D08](#adr-d08) |
| 选择理由 | Host 需要发起恢复时，让读请求产生可交付的限制响应，避免设备占着读等待 Host replay；保守限制已恢复的较早页，直到同一恢复义务完成。 |
| 模型假设/参数依据 | 已收 Pending 片段仍成功、缺片段仍 0xA；其他读在 AwaitingRestart/Replaying 时为 0x2。终点 program 成功即可解除，不额外等待该写响应交付；timeout、重新 page0 或更换 spare 不单独解除。 |
| 实现位置（成员/函数） | [恢复限制分支](../src/runtime/read_path.cpp)；[ReadRestrictionReason](../include/hbf_eventlab/runtime/read_path.hpp) |
| 验证测试与覆盖 | [T25](verification.md#t25)、[T26](verification.md#t26)、[T27](verification.md#t27)、[T30](verification.md#t30)：恢复前/中/后、较早失败合并、timeout 与 spare、解除时刻。 |
| 实现状态/验证状态 | 固定当前策略已实现；V1（给定所选恢复解释） |
| 已知限制 | 限制原因是内部语义枚举，不是已确定的 AXI RUSER 位编码；永久退役或已提交的旧内容放弃按各自状态处理，不称为恢复成功。 |

<a id="adr-d07"></a>

#### ADR-D07｜读限制从 ReplayState、准备状态、内容代及物理/逻辑事实推导，不另存可漂移的 blocked 状态。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受选定恢复解释与规范相关行为约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 本条记录权威状态到读视图的投影。BlockReadView 是查询结果，不是与 ReplayState 并行维护的第二套恢复状态机；只有当前 content generation 的写状态可约束当前逻辑内容。 |
| 规范依据/无直接依据说明 | 没有规范指定 read_view()；它落实 D05/D06 选定行为及 remap 后逻辑内容隔离。相关约束明确，不仅以“有助正确实现”为分类理由。 |
| 关联要求/条目 | REQ-READFORWARD、REQ-ZONE、[ADR-R08](#adr-r08)、[ADR-D05](#adr-d05)、[ADR-D06](#adr-d06)、[ADR-D12](#adr-d12) |
| 选择理由 | 减少状态同步点，防止 replay 已解除但读仍受限、换映射后仍套用旧恢复前缀等漂移。 |
| 模型假设/参数依据 | 读取 WritePath 的 replay/preparation 与 coordinator 的 content_generation；未绑定的新内容代不继承旧 Pending/replay；物理不可用与逻辑退役另从各自权威对象读取。 |
| 实现位置（成员/函数） | [WritePath::read_view()](../src/runtime/write_path.cpp)；[逻辑退役、read_view 与物理状态检查](../src/runtime/read_path.cpp) |
| 验证测试与覆盖 | [T25](verification.md#t25)、[T26](verification.md#t26)、[T27](verification.md#t27)、[T28](verification.md#t28)、[T29](verification.md#t29)：状态转换、合并终点、修复失败、remap 及 A→B→A。 |
| 实现状态/验证状态 | 已实现派生查询；V1 |
| 已知限制 | 依赖单模拟线程的一致观察；未来异步 Die 状态等应增加权威事实及派生规则，不能简单新增独立 read_blocked 标志。 |

<a id="adr-d08"></a>

#### ADR-D08｜读路径按已知事实分流，不从物理待替换标志推断更强错误。

| 字段 | 记录 |
|---|---|
| 规范关系 | 已退役地址 0x1 与首次不可恢复容量通知含义：直接规定；检测时机与临时读限制选择：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制（固定策略） |
| 分类边界说明 | 物理坏块、等待修复、逻辑退役及本次读数据错误是不同事实。ReadPath 当前不主动触发容量退役，也不产生 0x8；写 page0 修复耗尽时由 coordinator 提交逻辑退役。 |
| 规范依据/无直接依据说明 | §11.5.1.1/2/8、§11.5.2.1/8 给出相关状态语义；仅凭 physical unusable 不能证明本次应报 UECC/CECC、不可恢复容量或整 Die 阻塞。 |
| 关联要求/条目 | REQ-CAPACITY、[ADR-C02](#adr-c02)、[ADR-C04](#adr-c04)、[ADR-D06](#adr-d06)、[ADR-W11](#adr-w11) |
| 选择理由 | 沿用协调器统一提交退役事实；读路径保持最小范围，不凭物理标志隐式扩大故障。 |
| 模型假设/参数依据 | 通过前面的 Pending/恢复/内容代分支后，若实际物理目标 unusable，读报 0x2/PhysicalRepairRequired。首次写修复耗尽为 0x8/info1，之后读写该逻辑地址为 0x1。 |
| 实现位置（成员/函数） | [已退役检查](../src/runtime/read_path.cpp)；[物理目标不可用分支](../src/runtime/read_path.cpp)；[repair_block() 与首次退役错误](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T28](verification.md#t28)：没有 replay 义务的坏目标、无 spare 修复、首次写 0x8、后续读写 0x1 且无新 DLU/erase。 |
| 实现状态/验证状态 | 块级事实分流已接入；V1；读主动退役与广域异步退役未实现 |
| 已知限制 | 当前 0x2 是临时策略；未来故障模型应给出明确事实后再决定 0x4/0x5/0x6/0x8/0x9，不能用物理待替换标志代替它们的条件。 |

<a id="adr-d09"></a>

#### ADR-D09｜介质读在 submit 时登记候选，issue 时绑定目标并采样，finish_read 发布稳定结果。

| 字段 | 记录 |
|---|---|
| 规范关系 | 读 issue 时刻与结果快照：无直接规定，属于当前功能模型假设；结果生命周期：受规范相关响应约束，机制未指定 |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | submit 只建立候选和逻辑保护；真正的物理目标绑定、结果采样和有界结果槽预留发生在 issue。finish_read 发布已捕获结果；这些调用边界是本模型的实现机制，不声称对应规范规定的具体器件时刻。 |
| 规范依据/无直接依据说明 | REQ-RESPONSE 约束请求结果；规范不指定本模拟器 submit()/finish_read() 两个调用应对应何种器件时间点。 |
| 关联要求/条目 | REQ-MODEL、REQ-RESPONSE、[ADR-D04](#adr-d04)、[ADR-D10](#adr-d10) |
| 选择理由 | 为最小事件驱动闭环提供明确观察时点，避免把延迟通知误做延迟采样。 |
| 模型假设/参数依据 | issue 时重新检查当前状态并采样符号 token 或未写结果；之后的 program/failure 不追溯改变已捕获结果。示例时延不代表真实 tR、总线或 Cache 时延。 |
| 实现位置（成员/函数） | [介质采样与 Active 记录](../src/runtime/read_path.cpp)；[finish_read() 与 ASYNCFAULT 批注](../src/runtime/read_path.cpp)；[当前 issue 边界及后续调度说明](../include/hbf_eventlab/runtime/read_path.hpp) |
| 验证测试与覆盖 | [T33](verification.md#t33)：先读未写页再 program 仍返回该次未写结果；已捕获的成功数据不被后来物理故障改写。 |
| 实现状态/验证状态 | 固定功能模型已实现；V1 |
| 已知限制 | 当前 Device/Scheduler 已决定实际 issue 时刻，采样和完成仍是功能抽象；未模拟 sense 期间故障注入，不能推出真实器件所有在途故障都不影响结果。 |

<a id="adr-d10"></a>

#### ADR-D10｜读 ticket 固定目标到完成；有界结果所有权与 MOCS credit 保留至 Host 交付。

| 字段 | 记录 |
|---|---|
| 规范关系 | 每 command 一次响应：直接规定；lease 与响应副本：受规范约束，机制未指定；独立 ID 与关闭清理：无直接规定 |
| 条目性质 | 行为要求；实现机制；实现保障 |
| 分类边界说明 | 介质完成与 Host 响应交付是不同生命周期边界。finish_read() 先发布稳定结果，再释放物理 lease；交付前由有界存储持有数据，ReadPath 持有可移交句柄，command credit 仍占用。正常终止不靠析构模拟。 |
| 规范依据/无直接依据说明 | REQ-RESPONSE 与映射/数据正确性约束结果及在途访问；规范未指定 ReadOperationId、shared_ptr 或析构器。 |
| 关联要求/条目 | REQ-RESPONSE、REQ-ZONE、[ADR-H08](#adr-h08)、[ADR-W08](#adr-w08)、[ADR-D09](#adr-d09) |
| 选择理由 | 避免 remap/page0 erase 跨越尚未完成的读依赖；完成后的稳定结果允许物理目标释放，同时通过有界结果所有权和 credit 保留约束后续交付，见 ADR-LOOP-P06、ADR-LOOP-R07。 |
| 模型假设/参数依据 | ReadOperationId 与写 ticket 使用独立类型；未知/重复 finish 或交付被诊断。ReadPath 析构放弃自身账本项、释放 lease，依赖对象必须比路径活得更久。 |
| 实现位置（成员/函数） | [ReadOperation 与生命周期接口](../include/hbf_eventlab/runtime/read_path.hpp)；[析构清理](../src/runtime/read_path.cpp)；[finish_read()](../src/runtime/read_path.cpp)；[response_delivered()](../src/runtime/read_path.cpp) |
| 验证测试与覆盖 | [T24](verification.md#t24)、[T35](verification.md#t35)：在途阻止重启/remap、finish 后允许覆盖但旧响应稳定、重复完成拒绝、关闭释放资源。 |
| 实现状态/验证状态 | 已实现；V1 |
| 已知限制 | 协议取消、完整 AXI 适配和完整异常恢复未实现。显式 read_group 已限制组内 issue/交付，旧 axi_id 不自动参与；responses() 只暴露可交付的组头结果，最终有限 Egress/链路仲裁由 DeviceDriver 执行。 |

<a id="adr-d11"></a>

#### ADR-D11｜内部 erase 或映射门禁采用未接受的 Backpressured；已有 Pending 转发与补片继续排空。

| 字段 | 记录 |
|---|---|
| 规范关系 | 内部等待的接收边界：规范未明确边界下的项目解释；lease 门禁及排空：受规范相关约束，机制未指定 |
| 条目性质 | 行为要求（项目解释）；实现机制；实现保障 |
| 分类边界说明 | 正式接受前，普通 erase 尚未完成或新逻辑读保护被 quiesce 阻止时，路径返回 Backpressured。已 Taken 请求可留在 Device RX 等待重试；已正式接受的介质读还可作为候选等待窗口、顺序和资源。需要 Host replay 的读按 D06 生成可交付的 0x2，不以无限等待代替结果。 |
| 规范依据/无直接依据说明 | 规范给出读写响应及 remap 前置条件，未指定本 C++ 接口的 Backpressured 契约；具体选择需同时满足不读取旧逻辑内容和可排空在途工作的要求。 |
| 关联要求/条目 | REQ-READFORWARD、REQ-ZONE、[ADR-Z05](#adr-z05)、[ADR-W08](#adr-w08)、[ADR-D05](#adr-d05)、[ADR-D06](#adr-d06) |
| 选择理由 | 避免等待自身所阻止的 Host 恢复命令；保留已有 DLU 的补片和无须新物理 lease 的转发出口。 |
| 模型假设/参数依据 | Backpressured 不登记新 command、不占 credit、无 completion；Pending 命中或更早错误分支可先返回。普通 erase 等待不自动升级为 replay 限制。 |
| 实现位置（成员/函数） | [ErasePending 与 pin 门禁](../src/runtime/read_path.cpp)；[已有 DLU 复用 binding](../src/runtime/write_path.cpp)；[page0 排空门禁](../src/runtime/write_path.cpp) |
| 验证测试与覆盖 | [T31](verification.md#t31)、[T32](verification.md#t32)、[T24](verification.md#t24)：普通 erase、quiesce、Pending 转发/缺片段及补片、lease 排空。 |
| 实现状态/验证状态 | 背压、RX 重试与候选等待已接入；V1 |
| 已知限制 | 当前 Host/Device 已有 WaitKey 订阅与自动重试：NotTaken 由 Host 保留，Taken 后的 RX 等待由 Device 管理。仍不保证任意输入、任意策略的公平性或无死锁；已有响应、合法补片和必要 timeout 必须能继续推进，停滞由 Runner 显式报告。 |

<a id="adr-d12"></a>

#### ADR-D12｜discard/remap 后以新内容代隔离旧数据；尚未绑定新内容的防御性读返回 0x7。

| 字段 | 记录 |
|---|---|
| 规范关系 | 旧内容隔离：受规范约束，机制未指定；禁止访问期间的防御性 0x7：无直接规定，属于模型边界选择 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | Host 在 remap 前使相关数据无效，并负责之后的重写；当前 0x7 防御结果不表示 swap 擦除了物理介质，也不是规范要求违规 Host 访问必须得到该码。 |
| 规范依据/无直接依据说明 | §11.4、REQ-ZONE；Table12/§11.5.1.7 的正常未写页含义不能直接证明“含旧用户数据但未绑定的新内容代”也必须报 0x7。 |
| 关联要求/条目 | REQ-ZONE、[ADR-R08](#adr-r08)、[ADR-Z03](#adr-z03)、[ADR-Z04](#adr-z04)、[ADR-D07](#adr-d07) |
| 选择理由 | 避免 remap 后读到另一逻辑所有者的数据，也避免相同物理坐标再次出现时复活旧 replay 义务。 |
| 模型假设/参数依据 | 成功提交 remap 更新 content generation；当前代未绑定时不查询旧 Pending/介质 token，返回 0x7；新 page0 完成 erase 并重写后读取新内容。A→B→A 仍按内容代区分。 |
| 实现位置（成员/函数） | [read_view() 的内容代判定](../src/runtime/write_path.cpp)；[未绑定内容代的防御分支](../src/runtime/read_path.cpp)；[REMAP-FAILED-TARGET 批注](../src/runtime/capacity_retirement_coordinator.cpp) |
| 验证测试与覆盖 | [T29](verification.md#t29)：对方旧 token 隔离、往返映射不复活旧恢复义务、新起点失败前缀及重写后新数据。 |
| 实现状态/验证状态 | 防御性读取及内容代隔离已实现；V1；损坏目标 remap 仍有限制 |
| 已知限制 | RecoveryRequired 仍是 planner 的现有限制，不能解释为必须先重放已无效的旧内容。补偿策略声明见本文 ADR-C05/C06；discard 语义按当前模型边界记录。 |

### 同Tick因果、截止与事件生命周期

<a id="adr-loop-e01"></a>

#### ADR-LOOP-E01｜同一Tick内用因果轮次推进零延迟反馈，不随意增加Tick。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 轮次和事件容器是模拟器因果机制；不能把它算成规范规定的硬件流水级或固定时延。 |
| 规范依据／无直接依据说明 | 规范没有定义离散事件轮次；关联§5.4.1(6)的时限和[截止边界解释](#adr-loop-e02)。 |
| 关联要求／条目 | REQ-MODEL；[ADR-E01](#adr-e01)；[ADR-E04](#adr-e04) |
| 选择理由 | 允许后期阶段产生同刻较早阶段事件，同时保留确定顺序和非重入。 |
| 模型假设／参数依据 | 普通事件键为(Tick,round,phase,EventId)；同/较早phase入下一轮，较晚phase可留当前轮。 |
| 实现位置（函数／成员） | [EventId EventQueue::schedule_at](../src/infrastructure/event_queue.cpp#L5)；[bool EventQueue::run_next](../src/infrastructure/event_queue.cpp#L50) |
| 验证测试与覆盖 | 同刻正向/反向phase入队、零延迟反馈和取消/限额。<br>[生命周期：event_rounds](../CMakeLists.txt#L141)；[生命周期：event_cancel_limits](../CMakeLists.txt#L141) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 单模拟线程；不能用于推断真实硬件零时延。 |

<a id="adr-loop-e02"></a>

#### ADR-LOOP-E02｜截止Tick先处理正常因果闭包；只有正式接受并进入组装的补片才能救回DLU。

| 字段 | 记录 |
|---|---|
| 规范关系 | 规范歧义或未明确边界下的项目解释 |
| 条目性质 | 行为要求（项目解释） |
| 分类边界说明 | 超时要求有直接依据；恰好等时刻的裁决及“接受”的事务级位置是项目解释。 |
| 规范依据／无直接依据说明 | §5.4.1(6) p58、Table13 p61、§11.5.2.5 p122；未定义本模拟器多轮事件的等时刻裁决。 |
| 关联要求／条目 | REQ-TIMEOUT；[ADR-E01](#adr-e01)；[ADR-E02](#adr-e02) |
| 选择理由 | 反馈释放credit引发的同刻有效补片仍可完成；仅在链路/RX中不能冒充收齐。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [bool EventQueue::run_next](../src/infrastructure/event_queue.cpp#L50)；[WriteSubmission WritePath::submit](../src/runtime/write_path.cpp#L53)；[bool DeviceDriver::admit](../src/runtime/device_driver.cpp#L143) |
| 验证测试与覆盖 | credit释放的补片因果链可救回；仅RX驻留或链路未到不算已接受。<br>[生命周期：deadline_credit_chain](../CMakeLists.txt#L146)；[生命周期：deadline_resident_only](../CMakeLists.txt#L146)；[Device：deadline_link_boundary](../CMakeLists.txt#L132) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 无限同Tick事件链由运行限额诊断，不强制插入超时。 |

<a id="adr-loop-e03"></a>

#### ADR-LOOP-E03｜同Tick组装deadline冻结为一批，整批终结后才推进其反馈。

| 字段 | 记录 |
|---|---|
| 规范关系 | 规范歧义或未明确边界下的项目解释 |
| 条目性质 | 行为要求（项目解释）；实现保障 |
| 分类边界说明 | 同批互不挽救是确定性解释；冻结ID、批次预算和异常续执行是实现该解释的机制。 |
| 规范依据／无直接依据说明 | §5.4.1(6)、§11.5.2.5规定组装timeout；没有规定多个同刻DLU的回调执行规则。 |
| 关联要求／条目 | REQ-TIMEOUT；REQ-RESPONSE；[ADR-LOOP-E02](#adr-loop-e02)；[ADR-E03](#adr-e03)；[ADR-E07](#adr-e07) |
| 选择理由 | 避免先处理A的错误响应释放credit，改变同一时刻B本应得到的超时结果。 |
| 模型假设／参数依据 | 批次预算不足时不执行半批；回调异常后的剩余冻结动作继续先于正常反馈。 |
| 实现位置（函数／成员） | [while (!deadlines_.empty()](../src/infrastructure/event_queue.cpp#L63)；[void EventQueue::check_budget](../src/infrastructure/event_queue.cpp#L33) |
| 验证测试与覆盖 | 两个同刻DLU、整批预算、异常后剩余deadline先行。<br>[生命周期：deadline_batch](../CMakeLists.txt#L141)；[生命周期：deadline_two_dlus](../CMakeLists.txt#L146)；[生命周期：deadline_limits](../CMakeLists.txt#L141)；[生命周期：deadline_exception](../CMakeLists.txt#L141) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | run_next可能执行多个deadline回调；executed按真实callback数统计。 |

<a id="adr-loop-e04"></a>

#### ADR-LOOP-E04｜事件取消、候选身份和窗口timer版本阻止迟到工作作用于新上下文。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | 本条是宿主事件/句柄身份保护，不是block重写epoch或PEC。 |
| 规范依据／无直接依据说明 | 无直接OCP条款；服务既有一次响应及旧操作不得破坏新上下文的不变量。 |
| 关联要求／条目 | REQ-MODEL；[ADR-E03](#adr-e03)；[ADR-E05](#adr-e05)；[ADR-LOOP-W04](#adr-loop-w04) |
| 选择理由 | 地址、窗口、Cache槽可复用，而异步事件可能晚到；身份复核拒绝过期操作。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [ReadDispatchWindow::flush_due(ReadWindowWakeup](../src/policies/read_dispatch.cpp#L45)；[bool ReadPath::valid](../src/runtime/read_path.cpp#L91)；[bool EventQueue::cancel](../src/infrastructure/event_queue.cpp#L21) |
| 验证测试与覆盖 | 窗口成员变化、陈旧读/写候选与Cache引用失效；不是所有对象的穷举。<br>[生命周期：window_membership](../CMakeLists.txt#L146)；[生命周期：read_stale_candidate](../CMakeLists.txt#L144)；[生命周期：program_stale_candidate](../CMakeLists.txt#L145)；[Device：cache_pin_invalidation](../CMakeLists.txt#L131) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 各身份只覆盖自己的生命周期；不存在所有组件共享的全局epoch。 |

### 路径生命周期、依赖与定向唤醒

<a id="adr-loop-p01"></a>

#### ADR-LOOP-P01｜读接受只创建候选；显式issue才发射，完成与响应交付继续分开。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 规范给出读缓存、顺序与响应约束；候选/issue API和流水边界是本模型满足它们的组织方式。 |
| 规范依据／无直接依据说明 | §5.3.1 p56–57；结果与排序约束；不把提交和介质访问等同。 |
| 关联要求／条目 | REQ-RESPONSE；[ADR-D09](#adr-d09)；[ADR-D10](#adr-d10) |
| 选择理由 | 允许已接受读等待窗口、顺序和硬件资源，不提前占Bank或固定物理目标。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [ReadSubmission ReadPath::submit](../src/runtime/read_path.cpp#L70)；[ReadProbe ReadPath::probe](../src/runtime/read_path.cpp#L100)；[ReadPath::issue_impl](../src/runtime/read_path.cpp#L159)；[void ReadPath::finish_read](../src/runtime/read_path.cpp#L199) |
| 验证测试与覆盖 | 接受未占介质、issue回滚及Device自动读写。<br>[生命周期：read_accept_issue](../CMakeLists.txt#L142)；[生命周期：issue_rollback](../CMakeLists.txt#L140)；[Device：auto_write_read](../CMakeLists.txt#L127) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | Pending/已知错误可在接受侧终结，无需介质issue。 |

<a id="adr-loop-p02"></a>

#### ADR-LOOP-P02｜已接受未发射读持逻辑内容依赖，冲突重写/repair/remap需等待；已接受读可穿过后来的quiesce排空。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 保持被接受访问的内容与映射语义是约束；逻辑guard是所选保护表示，不等于物理lease。 |
| 规范依据／无直接依据说明 | §5.3.1读正确性、§5.4.1(5)重写和§11.4丢弃/remap约束；内部guard未规定。 |
| 关联要求／条目 | REQ-ZONE；[ADR-W08](#adr-w08)；[ADR-Z05](#adr-z05)；[ADR-C11](#adr-c11) |
| 选择理由 | 后移物理绑定后仍保护旧读；同时避免remap等旧读、旧读又等quiesce解除的互等。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [CapacityRetirementCoordinator::acquire_read](../src/runtime/capacity_retirement_coordinator.cpp#L239)；[CapacityRetirementCoordinator::pin_read](../src/runtime/capacity_retirement_coordinator.cpp#L101)；[WriteSubmission WritePath::submit](../src/runtime/write_path.cpp#L53)；[void ReadPath::complete](../src/runtime/read_path.cpp#L113) |
| 验证测试与覆盖 | 多个旧读、quiesce排空和Cache读后的重写。<br>[生命周期：guard_multiple_readers](../CMakeLists.txt#L140)；[生命周期：read_quiesce_drain](../CMakeLists.txt#L144)；[Device：cache_rewrite](../CMakeLists.txt#L129) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 未移交/未接受请求不持guard或物理lease；不能据此推导Host可违反zone数据无效前提。 |

<a id="adr-loop-p03"></a>

#### ADR-LOOP-P03｜介质读在issue时绑定当前物理目标并采样符号内容，完成时稳定结果并释放保护。

| 字段 | 记录 |
|---|---|
| 规范关系 | 绑定/保存：受规范约束，机制未指定；采样时刻：无直接规定 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 事务内容正确性与生命周期是约束；issue采样加固定完成延迟是功能模型，不是真实sense电路过程。 |
| 规范依据／无直接依据说明 | §5.3.1读结果与顺序约束；无规范规定“C++函数调用时复制PageData”。 |
| 关联要求／条目 | REQ-MODEL；REQ-RESPONSE；[ADR-D09](#adr-d09)；[ADR-D10](#adr-d10)；[ADR-M01](#adr-m01) |
| 选择理由 | 资源获得前不固定物理lease；采样后用已保护结果隔离后续修改。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [ReadPath::issue_impl](../src/runtime/read_path.cpp#L159)；[void ReadPath::finish_read](../src/runtime/read_path.cpp#L199)；[auto fill=cache_.begin_fill](../src/runtime/device_driver.cpp#L355) |
| 验证测试与覆盖 | issue时读值固定、后续变化不改已确定数据、lease释放。<br>[生命周期：issue_sampling](../CMakeLists.txt#L140)；[生命周期：issue_rollback](../CMakeLists.txt#L140)；[Device：rewrite_saved_read](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不模拟NAND cell、电荷、ECC或sense中途变化；Cache命中走独立保护路径。 |

<a id="adr-loop-p04"></a>

#### ADR-LOOP-P04｜排队读遇到恢复/不可用等新事实时重查并固定错误，释放旧内容依赖，不无限等待修复。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定（受已选恢复读解释约束） |
| 条目性质 | 实现机制 |
| 分类边界说明 | 沿用0x2等既有含义和项目优先级；这里新增的是未发射候选的重查与终结机制。 |
| 规范依据／无直接依据说明 | Table12、§11.5.1.2；Pending优先和恢复限制组合沿用[ADR-D05](#adr-d05)–D08。 |
| 关联要求／条目 | REQ-RESPONSE；[ADR-D06](#adr-d06)；[ADR-D07](#adr-d07)；[ADR-D08](#adr-d08)；[ADR-E07](#adr-e07) |
| 选择理由 | 防止旧读guard阻止repair而读又等待repair；已固定结果不能被后来修复改写。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [ReadPath::observe](../src/runtime/read_path.cpp#L36)；[bool ReadPath::resolve_terminal](../src/runtime/read_path.cpp#L121)；[void ReadPath::arm](../src/runtime/read_path.cpp#L133) |
| 验证测试与覆盖 | 等待中的Pending变化、恢复/退役重检与迟到错误。<br>[生命周期：read_failure_revalidation](../CMakeLists.txt#L144)；[生命周期：read_repair_revalidation](../CMakeLists.txt#L144)；[Device：retirement_loop](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不实现已发射NAND的通用取消，也不补齐未建模的广域异步故障码。 |

<a id="adr-loop-p05"></a>

#### ADR-LOOP-P05｜erase/program候选查询不改变业务状态；发射前复核路径条件并取得一次性ticket。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 收齐、erase完成、前页program成功等条件沿规范；probe和issue分离避免查询产生介质副作用。 |
| 规范依据／无直接依据说明 | §5.4.1、§11.5.2.6；原写顺序/auto-erase约束。 |
| 关联要求／条目 | REQ-WRITEORDER；[ADR-W06](#adr-w06)；[ADR-W07](#adr-w07)；[ADR-E05](#adr-e05) |
| 选择理由 | Scheduler可以检查/排序候选而不会提前start_program；旧context不能因地址复用重新发射。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [WritePath::probe_program](../src/runtime/write_path.cpp#L208)；[WritePath::issue_program](../src/runtime/write_path.cpp#L225)；[WritePath::issue_erase](../src/runtime/write_path.cpp#L188)；[void DeviceDriver::execute_media](../src/runtime/device_driver.cpp#L276) |
| 验证测试与覆盖 | 候选查询不推进写状态，非法/陈旧候选不能发射。<br>[生命周期：write_probes](../CMakeLists.txt#L145)；[生命周期：program_stale_candidate](../CMakeLists.txt#L145)；[生命周期：write_ready_notifications](../CMakeLists.txt#L145) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | probe就绪不代表Bank和Cache可用；两侧条件由Device协调。 |

<a id="adr-loop-p06"></a>

#### ADR-LOOP-P06｜成功读结果由有界存储持有，路径只持可移交句柄。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 正确保存待交付数据有行为约束；IReadResultStorage/lease是内存所有权实现，容量另见M02。 |
| 规范依据／无直接依据说明 | §5.3.1缓存与有序响应；§5.2.2适配器缓冲；不推出无限结果区。 |
| 关联要求／条目 | REQ-MODEL；REQ-RESPONSE；[ADR-D10](#adr-d10)；[ADR-LOOP-L06](#adr-loop-l06) |
| 选择理由 | 避免Parent隐含复制未计费payload，使Cache→Egress交接可明确释放。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [class ReadResultLease](../include/hbf_eventlab/interfaces/read_result_storage.hpp#L21)；[void ReadPath::transfer_result](../src/runtime/read_path.cpp#L185)；[void DeviceDriver::queue_responses](../src/runtime/device_driver.cpp#L370) |
| 验证测试与覆盖 | 结果空间不足和结果转移/释放的容量约束。<br>[生命周期：read_bounded_results](../CMakeLists.txt#L142)；[生命周期：pending_capacity](../CMakeLists.txt#L143)；[Device：tx_cache_pressure](../CMakeLists.txt#L129) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 句柄抽象不证明真实硬件有相同容量或移动实现。 |

<a id="adr-loop-p07"></a>

#### ADR-LOOP-P07｜read_group 的组头 issue 约束与返回顺序。

> 适用边界：“组头限制”不表示入口拒绝后续成员，也不表示后续成员不占用正式接受后的资源。

| 字段 | 记录 |
|---|---|
| 规范关系 | 同ID有序：直接规定；显式组与组头执行：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 规范约束返回顺序；本版只把组头普通读限制到取数/发射，后续成员仍可先进入 RX/MOCS，等待组头完成后再 issue/交付。这是有限结果槽和确定性排序的保守机制。 |
| 规范依据／无直接依据说明 | §5.3.1(5) p57；不同ID可乱序，同ID保持顺序。 |
| 关联要求／条目 | REQ-READDISPATCH；REQ-RESPONSE；[ADR-D10](#adr-d10)；[ADR-E06](#adr-e06) |
| 选择理由 | 同组可访问不同页，不能假定后请求必然复用前页；后序错误不抢占组头结果槽。 |
| 模型假设／参数依据 | group缺省无序；旧axi_id字段不自动转成group。 |
| 实现位置（函数／成员） | [class ReadOrderTracker](../include/hbf_eventlab/runtime/read_order_tracker.hpp#L11)；[ReadProbe ReadPath::probe](../src/runtime/read_path.cpp#L100)；[void ReadPath::response_delivered](../src/runtime/read_path.cpp#L217)；[void DeviceDriver::queue_responses](../src/runtime/device_driver.cpp#L370) |
| 验证测试与覆盖 | 同组顺序、不同页/Bank及后到终端错误。<br>[生命周期：group_channel_isolation](../CMakeLists.txt#L147)；[Device：ordered_pages](../CMakeLists.txt#L129)；[Device：ordered_late_error](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不模拟完整AXI编码；组头执行比只约束返回更保守，可能降低并行。 |

<a id="adr-loop-p08"></a>

#### ADR-LOOP-P08｜有序Pending成功转发/0xA组合仍报LIMIT-READ-001模型能力诊断。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设；实现保障 |
| 分类边界说明 | 规范并未禁止该组合；该诊断不是OCP错误码，也不是规范授予的拒绝行为。 |
| 规范依据／无直接依据说明 | §5.3.1有序返回与§5.4.3 Pending观察共同约束；有限缓冲下尚无完整策略。 |
| 关联要求／条目 | REQ-READFORWARD；REQ-SCOPE；[ADR-D05](#adr-d05)；[ADR-D10](#adr-d10)；[ADR-LOOP-L08](#adr-loop-l08)；[LIMIT-READ-001](scope_and_limitations.md#ordered-pending) |
| 选择理由 | 避免在等待组序时改变Pending观察或假装有无限缓存；先显式收窄可运行子集。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [ReadPath::observe](../src/runtime/read_path.cpp#L36)；[RunReport SimulationRunner::run](../src/runtime/simulation_runner.cpp#L13) |
| 验证测试与覆盖 | 路径、Device和Host闭环三层均报告不支持有序Pending；未声称已支持该场景。<br>[生命周期：pending_order_diagnostic](../CMakeLists.txt#L143)；[Device：pending_order_diagnostic](../CMakeLists.txt#L129)；[Host：ordered_pending](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 接受副作用前诊断；到达该情况后是ModelError保留现场，不是正常协议完成。 |

<a id="adr-loop-p09"></a>

#### ADR-LOOP-P09｜按原因和对象一次性订阅，版本复核避免漏唤醒，重复通知合并，回调后重查。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 这是离散模拟的前进性和效率机制，不是硬件必须发送这些通知。 |
| 规范依据／无直接依据说明 | 无直接规范条款；服务本项目已选背压与合法性等待。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-L02](#adr-loop-l02)；[ADR-LOOP-H09](#adr-loop-h09)；[ADR-LOOP-P05](#adr-loop-p05) |
| 选择理由 | 避免每Tick轮询或一有进展就重试所有请求；订阅只表示可能变得可接受。 |
| 模型假设／参数依据 | 多条件为OR；单模拟线程；多数键定向，但部分共享结果空间通知使用channel-0键作广播重查。 |
| 实现位置（函数／成员） | [WaitObservation AdmissionWaitRegistry::observe](../src/runtime/admission_wait_registry.cpp#L7)；[WaitId AdmissionWaitRegistry::subscribe](../src/runtime/admission_wait_registry.cpp#L32)；[void AdmissionWaitRegistry::queue](../src/runtime/admission_wait_registry.cpp#L18)；[bool AdmissionWaitRegistry::cancel](../src/runtime/admission_wait_registry.cpp#L45) |
| 验证测试与覆盖 | 定向对象、检查/订阅竞态、取消、回调中再次订阅。<br>[生命周期：wait_targeting](../CMakeLists.txt#L142)；[生命周期：wait_race_cancel](../CMakeLists.txt#L142)；[生命周期：wait_reregister](../CMakeLists.txt#L142) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不是完整事件依赖索引优化：部分候选仍扫描；无多线程同步保证。 |

<a id="adr-loop-p10"></a>

#### ADR-LOOP-P10｜物理lease析构只更新计数与dirty标记，在稳定边界统一发布释放通知。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | lease存在为正确性服务，但本条专指无重入析构和通知发布的宿主保障。 |
| 规范依据／无直接依据说明 | 无直接OCP条款；关联[ADR-C11](#adr-c11)的物理使用者排空。 |
| 关联要求／条目 | REQ-MODEL；[ADR-C11](#adr-c11)；[ADR-LOOP-P09](#adr-loop-p09) |
| 选择理由 | 避免析构或提交中间重入外部逻辑，同时减少全物理块扫描。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [CapacityRetirementCoordinator::publish_releases](../src/runtime/capacity_retirement_coordinator.cpp#L252)；[PhysicalMappingLease::~PhysicalMappingLease()](../src/runtime/capacity_retirement_coordinator.cpp#L31) |
| 验证测试与覆盖 | 物理lease归零及zone等待方通知。<br>[生命周期：physical_release_notification](../CMakeLists.txt#L145)；[生命周期：physical_zone_wakeup](../CMakeLists.txt#L140) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 直接使用lease的自定义驱动必须遵守publish契约；并非在线硬件lease协议。 |

### 拓扑、资源、Cache与调度

<a id="adr-loop-r01"></a>

#### ADR-LOOP-R01｜显式Core Channel路由接在UCIe与Die之间，当前无限带宽、零延迟、不争用。

| 字段 | 记录 |
|---|---|
| 规范关系 | 拓扑受规范约束；无限带宽/零延迟无直接规定 |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | 第4章拓扑与理想传输假设分开；缺少可用的速率依据不等于物理Core链路没有开销。 |
| 规范依据／无直接依据说明 | 第4章Base/Core组织；§5.1.2数值是UCIe module的吞吐，不能直接赋给Core Channel。 |
| 关联要求／条目 | REQ-ADDR；REQ-MODEL；[ADR-LOOP-R02](#adr-loop-r02)；[ADR-LOOP-M02](#adr-loop-m02) |
| 选择理由 | 先保持拓扑和路由扩展位置，不为未知内部带宽编造精确值。 |
| 模型假设／参数依据 | 默认每UCIe channel一个Core Channel；空路由表使用die%count，不增加Die/容量。 |
| 实现位置（函数／成员） | [CoreChannelTopology::CoreChannelTopology](../src/architecture/topology_configuration.cpp)；[CoreChannelTopology::core_channel](../include/hbf_eventlab/architecture/topology_configuration.hpp)；[topology_.core_channel](../src/runtime/device_driver.cpp#L329) |
| 验证测试与覆盖 | 拓扑配置和独立Bank并行；没有真实Core带宽验证。<br>[Device：configuration](../CMakeLists.txt#L131)；[Device：bank_parallel](../CMakeLists.txt#L127) |
| 实现状态／验证状态 | 已实现于当前支持子集；V2。 |
| 已知限制 | 暂不提供可替换Core链路执行接口，未来需要新增争用模型。 |

<a id="adr-loop-r02"></a>

#### ADR-LOOP-R02｜每个物理Bank同一时刻最多一个read/program/erase，独立Bank可并行；不全程锁祖先Die。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定（保守资源基线） |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | Bank并行组织有规范背景；把三类操作全程串行化及不设Die全程锁是所选资源模型。 |
| 规范依据／无直接依据说明 | 第4章Bank组织、§5.3.1(5b/6)同Bank sense约束；不据此声称规范禁止所有Bank内部流水。 |
| 关联要求／条目 | REQ-MODEL；REQ-READDISPATCH；[ADR-LOOP-R01](#adr-loop-r01)；[ADR-LOOP-R03](#adr-loop-r03) |
| 选择理由 | 提供可解释的最小争用边界，避免父资源锁无意消除Bank并行。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [class DeviceRuntimeState](../include/hbf_eventlab/runtime/device_runtime_state.hpp#L12)；[void DeviceDriver::execute_media](../src/runtime/device_driver.cpp#L276) |
| 验证测试与覆盖 | 同Bank串行和异Bank并行。<br>[Device：bank_parallel](../CMakeLists.txt#L127)；[Device：bank_serial](../CMakeLists.txt#L127) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | Bank释放发生在介质完成；缓存保护、TX和MOCS有各自更晚的释放边界。 |

<a id="adr-loop-r03"></a>

#### ADR-LOOP-R03｜NAND选择、UCIe响应仲裁和窗口释放分属不同策略，Device先过滤合法候选。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 可配置策略；实现机制 |
| 分类边界说明 | 顺序、组装与资源正确性不可替换；策略只在满足这些规则的集合中选优先级。 |
| 规范依据／无直接依据说明 | §5.3.1顺序/Cache约束、§5.3.2调度空间、§5.4.1写条件。 |
| 关联要求／条目 | REQ-READDISPATCH；REQ-WRITEORDER；[ADR-LOOP-R04](#adr-loop-r04)；[ADR-LOOP-W03](#adr-loop-w03)；[ADR-LOOP-L04](#adr-loop-l04) |
| 选择理由 | 区分阵列与链路资源；高优先级受阻时仍可发射其他合法Bank，避免策略改变合法性。 |
| 模型假设／参数依据 | 默认NAND FCFS与响应FIFO按accepted_sequence及稳定ID；请求方向选择由Host策略完成。 |
| 实现位置（函数／成员） | [class INandSchedulingPolicy](../include/hbf_eventlab/interfaces/execution_policy.hpp#L15)；[class IUcieArbitrationPolicy](../include/hbf_eventlab/interfaces/execution_policy.hpp#L21)；[void DeviceDriver::execute_media](../src/runtime/device_driver.cpp#L276)；[void DeviceDriver::start_response](../src/runtime/device_driver.cpp#L396) |
| 验证测试与覆盖 | 替换/错误策略、同Bank顺序防越过以及Host候选护栏。<br>[Device：policies](../CMakeLists.txt#L132)；[Device：invalid_policy](../CMakeLists.txt#L132)；[Device：sense_order_policy](../CMakeLists.txt#L133)；[Host：policy_guard](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 窗口/Bank候选仍有线性扫描；接口存在不代表任意插件合规。 |

<a id="adr-loop-r04"></a>

#### ADR-LOOP-R04｜同 Bank sense 严格有序，以正式接受序作为当前实现基线。

| 字段 | 记录 |
|---|---|
| 规范关系 | 严格sense顺序：直接规定；采用正式接受序：项目解释 |
| 条目性质 | 行为要求；行为要求（项目解释） |
| 分类边界说明 | §5.3.1(6)是固定约束；“请求顺序”落到哪个抽象接收边界，需要本版本明确。 |
| 规范依据／无直接依据说明 | §5.3.1(5b/6) p57；规范没有规定本模拟器accepted_sequence字段。 |
| 关联要求／条目 | REQ-READDISPATCH；[ADR-LOOP-R03](#adr-loop-r03)；[ADR-LOOP-P07](#adr-loop-p07) |
| 选择理由 | 避免替换Scheduler后让后到sense越过同Bank旧读，包含旧读尚受跨Bank组序阻挡的情况。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [bool DeviceDriver::prior_sense](../src/runtime/device_driver.cpp#L265)；[t.accepted_sequence=++stats_.accepted](../src/runtime/device_driver.cpp#L177)；[void DeviceDriver::notify_sense_order](../src/runtime/device_driver.cpp#L262) |
| 验证测试与覆盖 | 逆序偏好策略仍不能越过同Bank旧sense；旧读被组序阻挡时也保序。<br>[Device：sense_order_policy](../CMakeLists.txt#L133)；[Device：sense_order_blocked](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不把不同Bank排序合并为全局FIFO；Cache合法命中可按既定规则服务。 |

<a id="adr-loop-r05"></a>

#### ADR-LOOP-R05｜每Bank两个页Cache；合法命中不等待读窗口，发射sense前再次查Cache。

| 字段 | 记录 |
|---|---|
| 规范关系 | 双缓存和及时命中服务：直接规定；查询流水组织：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 两个buffer和至少两页能力是规范内容；两固定4KiB槽、进入和issue前查询是参考实现。 |
| 规范依据／无直接依据说明 | §5.3.1(7/8) p57。 |
| 关联要求／条目 | REQ-READDISPATCH；[ADR-D04](#adr-d04)；[ADR-LOOP-R06](#adr-loop-r06)；[ADR-LOOP-W01](#adr-loop-w01) |
| 选择理由 | 处理入队miss后来变hit的情况，减少重复sense，且先检查Pending/组序而不返回旧cache数据。 |
| 模型假设／参数依据 | 两槽各4KiB；cache_access默认100ps是模型延迟而非“立即”等于零时间。 |
| 实现位置（函数／成员） | [bool BankCache::contains](../src/runtime/bank_cache.cpp#L8)；[bool DeviceDriver::try_cache](../src/runtime/device_driver.cpp#L221)；[if(cache_.contains(*p.target))](../src/runtime/device_driver.cpp#L316) |
| 验证测试与覆盖 | 立即合法命中、窗口miss变hit、Pending重检不被Cache绕过。<br>[Device：cache_hit](../CMakeLists.txt#L128)；[Device：miss_becomes_hit](../CMakeLists.txt#L129)；[Device：pending_recheck](../CMakeLists.txt#L132) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 只存DataToken页；不模拟物理Cache端口电路。 |

<a id="adr-loop-r06"></a>

#### ADR-LOOP-R06｜Cache替换仅选择未填充、未被结果引用的槽，无效槽优先，随后选最久未用。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 两缓存及已接受读结果正确性约束替换；无效优先/LRU不是规范指定策略。 |
| 规范依据／无直接依据说明 | §5.3.1(7)的双缓存及第三页可能驱逐背景；替换算法未指定。 |
| 关联要求／条目 | REQ-READDISPATCH；REQ-MODEL；[ADR-LOOP-R05](#adr-loop-r05)；[ADR-LOOP-R07](#adr-loop-r07) |
| 选择理由 | 避免UCIe拥塞时覆盖尚未移交的读结果；稳定地选一个可替换槽。 |
| 模型假设／参数依据 | age为访问序号；refs保护与tag有效性分开。 |
| 实现位置（函数／成员） | [std::optional<CacheFill> BankCache::begin_fill](../src/runtime/bank_cache.cpp#L18)；[ReadResultLease BankCache::reference](../src/runtime/bank_cache.cpp#L48) |
| 验证测试与覆盖 | TX拥塞使受引用槽不可覆盖；失效后旧结果仍可读。未比较LRU优劣。<br>[Device：tx_cache_pressure](../CMakeLists.txt#L129)；[Device：cache_pin_invalidation](../CMakeLists.txt#L131) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 当前替换算法硬编码，无独立CacheReplacementPolicy接口，不能标为已可替换策略；相对其他算法的收益和选择依据尚未比较验证。 |

<a id="adr-loop-r07"></a>

#### ADR-LOOP-R07｜sense必须先有Cache目的槽；完成释放Bank，结果经有限Egress交接后才释放Cache引用。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 结果不能丢失是约束；“Cache保护到Egress可用”的背压传播是所选有界数据流。 |
| 规范依据／无直接依据说明 | §5.3.1双Cache和读返回；§5.2.2适配器缓冲；未给出本项目深度与交接时刻。 |
| 关联要求／条目 | REQ-MODEL；REQ-RESPONSE；[ADR-LOOP-P06](#adr-loop-p06)；[ADR-LOOP-R06](#adr-loop-r06)；[ADR-LOOP-L06](#adr-loop-l06) |
| 选择理由 | 回答NAND已完成但UCIe忙时结果在哪里；满TX可最终阻挡同Bank新miss。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [if(!cache_.can_fill(bank))](../src/runtime/device_driver.cpp#L319)；[void DeviceDriver::queue_responses](../src/runtime/device_driver.cpp#L370)；[void ReadPath::transfer_result](../src/runtime/read_path.cpp#L185) |
| 验证测试与覆盖 | 有限TX、两槽压力和旧结果保留后的重写。<br>[Device：tx_cache_pressure](../CMakeLists.txt#L129)；[Device：rewrite_saved_read](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 其他有资源的Bank仍可推进；无无限结果暂存副本。 |

<a id="adr-loop-r08"></a>

#### ADR-LOOP-R08｜erase/program/failure/remap改变介质时失效Cache标签，但保留受保护的已确定结果。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 不能把旧内容作为新命中是正确性约束；失效粒度和保留引用机制由模型选择。 |
| 规范依据／无直接依据说明 | §5.4.1重写、§11.4 remap数据无效及§5.3.1读结果语义。 |
| 关联要求／条目 | REQ-RESPONSE；REQ-ZONE；[ADR-Z06](#adr-z06)；[ADR-D12](#adr-d12)；[ADR-LOOP-R07](#adr-loop-r07) |
| 选择理由 | 区分“标签不可用于新读”和“旧响应数据仍要交付”，避免两者共用有效位造成丢失。 |
| 模型假设／参数依据 | erase/fail失效块，成功program失效页，repair/remap当前全Cache标签失效。 |
| 实现位置（函数／成员） | [void BankCache::invalidate(](../src/runtime/bank_cache.cpp#L81)；[void BankCache::invalidate_all](../src/runtime/bank_cache.cpp#L89)；[const auto outcome=outcome_(op)](../src/runtime/device_driver.cpp#L346)；[DeviceDriver::commit_swap](../src/runtime/device_driver.cpp#L462) |
| 验证测试与覆盖 | Cache pin失效、remap后标签刷新以及已保存读结果。<br>[Device：cache_pin_invalidation](../CMakeLists.txt#L131)；[Device：remap_cache](../CMakeLists.txt#L131)；[Device：rewrite_saved_read](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 全局失效比必要范围更保守；未模拟失效消息代价。 |

### 读窗口接入

<a id="adr-loop-w01"></a>

#### ADR-LOOP-W01｜每UCIe channel一个读累积窗口，可覆盖其下多个Die/Bank。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | Regular累积并优化调度有依据；按channel而非Bank划分窗口是本项目组织选择。 |
| 规范依据／无直接依据说明 | §5.3.2.1 p57；不能从“best performance”直接证明只有一种窗口范围。 |
| 关联要求／条目 | REQ-READDISPATCH；[ADR-D02](#adr-d02)；[ADR-LOOP-W02](#adr-loop-w02)；[ADR-LOOP-R03](#adr-loop-r03) |
| 选择理由 | 允许同通道内资源并行和Cache局部性选择，同时不扩展为跨channel完成屏障。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [struct Channel {](../include/hbf_eventlab/runtime/device_driver.hpp#L91)；[bool DeviceDriver::admit](../src/runtime/device_driver.cpp#L143) |
| 验证测试与覆盖 | 不同channel窗口隔离，集合中不同Bank可并行。<br>[Device：window_channel_isolation](../CMakeLists.txt#L128)；[Device：bank_parallel](../CMakeLists.txt#L127) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 窗口只保存需要介质服务的成员；候选物理路由仍在发射前复核。 |

<a id="adr-loop-w02"></a>

#### ADR-LOOP-W02｜接受B立即关闭截至自身的当前集合；已转发或0xA的有效B也触发边界，NotTaken/未接受无副作用。

| 字段 | 记录 |
|---|---|
| 规范关系 | 规范歧义或未明确边界下的项目解释 |
| 条目性质 | 行为要求（项目解释）；实现机制 |
| 分类边界说明 | 本模型将 RRBBR 解释为 RRB、B 和末尾等待释放的 R 三个集合；Pending B 仍触发窗口边界属于项目解释。 |
| 规范依据／无直接依据说明 | §5.3.2.2 p57的正文/示例边界并不完全清晰；§5.4.3给出Pending结果。 |
| 关联要求／条目 | REQ-READDISPATCH；REQ-READFORWARD；[ADR-D01](#adr-d01)；[ADR-D05](#adr-d05)；[ADR-LOOP-W01](#adr-loop-w01) |
| 选择理由 | 提示边界与是否产生NAND成员分开，让B表达已接受请求的调度边界。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [ReadDispatchWindow::accept](../src/policies/read_dispatch.cpp#L17)；[DeviceDriver::admit — window.accept](../src/runtime/device_driver.cpp#L193) |
| 验证测试与覆盖 | B与timer、Pending命中B、缺片B和未接受B的无副作用。未穷举所有错误命令组合。<br>[Device：window_batch_timer](../CMakeLists.txt#L128)；[Device：batch_pending](../CMakeLists.txt#L128)；[Device：batch_missing](../CMakeLists.txt#L128)；[Device：rejected_batch_no_effect](../CMakeLists.txt#L130) |
| 实现状态／验证状态 | 已实现于当前支持子集；V2。 |
| 已知限制 | 未实现真实AXI user位编码；当前有效hint的已接受终端读也可触发边界，未穷举所有错误组合。 |

<a id="adr-loop-w03"></a>

#### ADR-LOOP-W03｜窗口释放的是调度资格；成员可分时发射完成，不要求整批同时完成或跨集合屏障。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 规范要求调度读集合；不要求把不同Bank串成整批完成事务。 |
| 规范依据／无直接依据说明 | §5.3.2，结合§5.3.1 Bank/顺序约束。 |
| 关联要求／条目 | REQ-READDISPATCH；[ADR-LOOP-W01](#adr-loop-w01)；[ADR-LOOP-W02](#adr-loop-w02)；[ADR-LOOP-R03](#adr-loop-r03) |
| 选择理由 | 资源受阻成员保留已释放资格，合法其他Bank可继续，避免重新累积等待。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [void DeviceDriver::release_batches](../src/runtime/device_driver.cpp#L198)；[void DeviceDriver::execute_media](../src/runtime/device_driver.cpp#L276) |
| 验证测试与覆盖 | 阈值释放、独立Bank推进和同Bank受阻。<br>[Device：bank_parallel](../CMakeLists.txt#L127)；[Device：window_threshold](../CMakeLists.txt#L128)；[Device：sense_order_blocked](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 同Bank sense/组序硬限制仍可能让部分成员等待。 |

<a id="adr-loop-w04"></a>

#### ADR-LOOP-W04｜窗口用主动timer和深度阈值推进；成员变化取消/重建timer，旧版本不释放新窗口。

| 字段 | 记录 |
|---|---|
| 规范关系 | 累积/调度受规范约束；主动事件与版本属于实现保障 |
| 条目性质 | 可配置策略；实现保障 |
| 分类边界说明 | 等待/阈值可配置，具体默认值非规范；旧timer身份不涉及NAND重写代次。 |
| 规范依据／无直接依据说明 | §5.3.2提及timer与集合；未规定本模拟器阈值、期限或generation。 |
| 关联要求／条目 | REQ-READDISPATCH；REQ-MODEL；[ADR-D02](#adr-d02)；[ADR-E03](#adr-e03)；[ADR-LOOP-E04](#adr-loop-e04) |
| 选择理由 | 最后一条Regular没有后续到达也能前进，且不因旧timer误触发新集合。 |
| 模型假设／参数依据 | 默认窗口10000ps、阈值32；阈值0可关闭数量触发。 |
| 实现位置（函数／成员） | [Tick WindowDispatchPolicy::deadline](../src/policies/read_dispatch.cpp#L6)；[ReadDispatchWindow::flush_due(ReadWindowWakeup](../src/policies/read_dispatch.cpp#L45)；[void DeviceDriver::update_timer](../src/runtime/device_driver.cpp#L206) |
| 验证测试与覆盖 | 成员删除/旧timer、尾Regular定时释放和数量触发。<br>[生命周期：window_membership](../CMakeLists.txt#L146)；[Device：window_batch_timer](../CMakeLists.txt#L128)；[Device：window_threshold](../CMakeLists.txt#L128) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 窗口timer属于正常Arrival事件，不使用组装Deadline批次。 |

### 事务级链路、双向缓冲与响应交付

<a id="adr-loop-l01"></a>

#### ADR-LOOP-L01｜Taken时预留RX并开始传输，完整到达转驻留；源在NotTaken时继续持有请求。

| 字段 | 记录 |
|---|---|
| 规范关系 | 流控/缓冲受规范约束，机制未指定；预留边界为模型选择 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 规范明确适配器缓冲与FIFO空间反馈；Taken及预留完整事务是抽象，不是逐flit信用。 |
| 规范依据／无直接依据说明 | §5.2.1–5.2.3 p38–40。 |
| 关联要求／条目 | REQ-MODEL；REQ-MOCS；[ADR-LOOP-L02](#adr-loop-l02)；[ADR-LOOP-L06](#adr-loop-l06)；[ADR-LOOP-H09](#adr-loop-h09) |
| 选择理由 | 先预留避免多个在途请求到达时超卖；链路忙返回NotTaken而非藏无限发送队列。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [HandoffResult DeviceDriver::try_submit](../src/runtime/device_driver.cpp#L76)；[class BufferBudget](../include/hbf_eventlab/runtime/link_buffers.hpp#L6) |
| 验证测试与覆盖 | 在途RX预留、重复移交拒绝与传输延迟。<br>[Device：rx_reservation](../CMakeLists.txt#L127)；[Device：duplicate_handoff](../CMakeLists.txt#L131)；[Device：link_latency](../CMakeLists.txt#L132) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | RX汇总条目包含预留和驻留，不能与in_flight统计直接相加当唯一请求数。 |

<a id="adr-loop-l02"></a>

#### ADR-LOOP-L02｜RX 驻留、MOCS 和建模错误响应的边界。

| 字段 | 记录 |
|---|---|
| 规范关系 | 计数对象受规范约束；接收门限为已选项目解释 |
| 条目性质 | 实现机制；行为要求（项目解释） |
| 分类边界说明 | RX 驻留属于链路/适配器状态，不占正式 MOCS。进入路径并通过 MOCS 后，建模的 Host 语义错误仍需占用一个父 command credit 并等待响应交付；API 能力/形状诊断在模型入口直接报错，不产生协议 completion。 |
| 规范依据／无直接依据说明 | §5.9.2.1 p70、§5.9.2.3 p71–72；规范未明确MOCS满后的专用错误响应。 |
| 关联要求／条目 | REQ-MOCS；REQ-RESPONSE；[ADR-H04](#adr-h04)；[ADR-H08](#adr-h08)；[ADR-H09](#adr-h09)；[ADR-LOOP-L01](#adr-loop-l01) |
| 选择理由 | 区分链路流控与设备outstanding，MOCS不足只重试Ingress，不重复发链路。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [bool DeviceDriver::admit](../src/runtime/device_driver.cpp#L143)；[SubmitResult CommandAdmission::try_accept](../src/runtime/command_admission.cpp#L48) |
| 验证测试与覆盖 | MOCS满时驻留RX、可行事务越过受阻者、Host部分DLU压力。<br>[Device：mocs_ingress](../CMakeLists.txt#L127)；[Device：ingress_bypass](../CMakeLists.txt#L130)；[Host：mocs_partial](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | Pending DLU名额仍有单独错误语义；不把RX容量当MOCS。 |

<a id="adr-loop-l03"></a>

#### ADR-LOOP-L03｜写metadata和DataToken分开存放，但Host一次提交关联事务，不拆成AW/W两条命令。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定（事务级模型抽象） |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | 规范确实区分AXI AW/W；模型合并关联提交是省略总线细节，不宣称已实现该握手协议。 |
| 规范依据／无直接依据说明 | §5.2.1 p39的AW/W/B；§5.4.1(1)每写command一次响应。 |
| 关联要求／条目 | REQ-MODEL；[ADR-H01](#adr-h01)；[ADR-H02](#adr-h02)；[ADR-LOOP-H02](#adr-loop-h02) |
| 选择理由 | 保留控制和数据所有权，同时避免把一条写命令计成两份MOCS。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [struct LocalTransaction](../include/hbf_eventlab/interfaces/host_port.hpp#L9)；[t.input.data.clear()](../src/runtime/device_driver.cpp#L176)；[LocalTransaction tx](../src/host/host_driver.cpp#L221) |
| 验证测试与覆盖 | 分离metadata/payload仍一条command；64B与显式burst记账差异。<br>[Device：split_write](../CMakeLists.txt#L127)；[Host：burst_compare](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不覆盖AW/W乱序到达、逐拍VALID/READY、flit打包或AXI协议验证。 |

<a id="adr-loop-l04"></a>

#### ADR-LOOP-L04｜使用UCIe有效GB/s计算序列化服务时间；速率按物理channel预算，不乘虚拟AXI接口数。

| 字段 | 记录 |
|---|---|
| 规范关系 | 速率档来源：直接规定；事务成本/独立方向映射：模型假设 |
| 条目性质 | 行为要求；模型／输入假设 |
| 分类边界说明 | 48/94/188是每UCIe module系统读写吞吐来源；不能据此证明每个模拟事务固定控制字节数或内部全双工细节。 |
| 规范依据／无直接依据说明 | §5.1.2.1 p29、§5.1.2.2 p32、§5.1.2.3 p35。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-L05](#adr-loop-l05)；[ADR-LOOP-M01](#adr-loop-m01) |
| 选择理由 | 有效带宽已含对应效率，不再次扣除；把源数据和新增服务成本假设分开。 |
| 模型假设／参数依据 | 默认48GB/s；ceil(service_bytes*1000/rate) ps；控制服务量16B是模型假定。 |
| 实现位置（函数／成员） | [Tick UcieExecutionConfiguration::serialization](../src/architecture/execution_configuration.cpp)；[HandoffResult DeviceDriver::try_submit](../src/runtime/device_driver.cpp#L76)；[void DeviceDriver::start_response](../src/runtime/device_driver.cpp#L396) |
| 验证测试与覆盖 | 整数服务时间计算和不同channel带宽并行；没有硬件校准。<br>[Device：configuration](../CMakeLists.txt#L131)；[Device：multi_channel_bandwidth](../CMakeLists.txt#L134) |
| 实现状态／验证状态 | 已实现于当前支持子集；V2。 |
| 已知限制 | 非上述档位允许研究；方向独立、channel↔module一对一是当前抽象，未做吞吐校准。 |

<a id="adr-loop-l05"></a>

#### ADR-LOOP-L05｜序列化结束即可服务下一事务，传播延迟可重叠；缓冲保存到对应到达/交付边界。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定（时延分解模型） |
| 条目性质 | 模型／输入假设；实现机制 |
| 分类边界说明 | 长度/有效带宽只给服务时间；固定传播、方向资源释放和槽驻留是模型执行关系。 |
| 规范依据／无直接依据说明 | 关联§5.1.2/§5.2，未规定此事务级分解和默认固定延迟。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-L01](#adr-loop-l01)；[ADR-LOOP-L04](#adr-loop-l04)；[ADR-LOOP-L06](#adr-loop-l06) |
| 选择理由 | 避免把完整端到端延迟全当带宽占用，也避免过早释放在途存储。 |
| 模型假设／参数依据 | 请求/返回固定延迟各100ps；每方向串行服务。 |
| 实现位置（函数／成员） | [const auto serialized=after(events_.now(),config_.serialization(service));](../src/runtime/device_driver.cpp)；[void DeviceDriver::start_response](../src/runtime/device_driver.cpp#L396) |
| 验证测试与覆盖 | 服务完成与传播到达分离、在途仍占RX。<br>[Device：link_latency](../CMakeLists.txt#L132)；[Device：rx_reservation](../CMakeLists.txt#L127) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 未建模传播内部细分阶段，固定延迟不是校准结果。 |

<a id="adr-loop-l06"></a>

#### ADR-LOOP-L06｜RX/TX都有限，条目与payload字节分开记账；成功数据和元数据响应均占返回条目。

| 字段 | 记录 |
|---|---|
| 规范关系 | 有限适配器有依据；聚合预算和深度无直接规定 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 规范给出各AXI通道的少量适配器缓冲；本版按请求/返回方向聚合，不能当逐通道电路等价证明。 |
| 规范依据／无直接依据说明 | §5.2.2–5.2.3 p39–40。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-P06](#adr-loop-p06)；[ADR-LOOP-R07](#adr-loop-r07)；[ADR-LOOP-L01](#adr-loop-l01)；[ADR-LOOP-L07](#adr-loop-l07) |
| 选择理由 | 让响应拥塞真实地反压Cache/读发射，避免错误响应不计资源。 |
| 模型假设／参数依据 | RX每channel16项/16384B；TX8项/512B；读成功64B payload，错误/写响应payload计0。 |
| 实现位置（函数／成员） | [class BufferBudget](../include/hbf_eventlab/runtime/link_buffers.hpp#L6)；[class EgressBuffer](../include/hbf_eventlab/runtime/link_buffers.hpp#L23)；[void DeviceDriver::queue_responses](../src/runtime/device_driver.cpp#L370) |
| 验证测试与覆盖 | TX/Cache满、Pending转发空间和有序错误等待；尚未穷举所有深度组合。<br>[Device：tx_cache_pressure](../CMakeLists.txt#L129)；[Device：pending_tx_full](../CMakeLists.txt#L130)；[Device：ordered_late_error](../CMakeLists.txt#L133) |
| 实现状态／验证状态 | 已实现于当前支持子集；V2。 |
| 已知限制 | 错误元数据可暂留MOCS约束的Parent；不额外假定MOCS大小的无限数据区。 |

<a id="adr-loop-l07"></a>

#### ADR-LOOP-L07｜交付恰好一次释放对应credit/返回槽，Host反馈在后续同Tick事件执行；读写响应保留独立类型。

| 字段 | 记录 |
|---|---|
| 规范关系 | 一次响应受规范约束；后续事件回调与variant为实现机制 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 写non-posted和一次响应有直接依据；资源释放及异步回调边界是本模型落实生命周期的方式。 |
| 规范依据／无直接依据说明 | §5.4.1(1/2)及Table12/13；读写状态数值不能混解释。 |
| 关联要求／条目 | REQ-RESPONSE；REQ-MOCS；[ADR-H02](#adr-h02)；[ADR-H08](#adr-h08)；[ADR-E06](#adr-e06)；[ADR-LOOP-H09](#adr-loop-h09) |
| 选择理由 | 先完成设备清理再反馈，防止回调重入半更新状态；无需虚构额外Host ACK。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [void DeviceDriver::deliver](../src/runtime/device_driver.cpp#L416)；[using DeviceResponse](../include/hbf_eventlab/interfaces/host_port.hpp#L10) |
| 验证测试与覆盖 | 响应反馈、callback遇运行限额、端到端计数与排空。<br>[Device：response_feedback](../CMakeLists.txt#L131)；[Device：callback_limit](../CMakeLists.txt#L134)；[Host：normal](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | Host消费反馈前可以仍有未结索引；它不表示Device credit仍未释放。 |

<a id="adr-loop-l08"></a>

#### ADR-LOOP-L08｜Pending在正式接受侧观察；成功转发先预留结果空间，未接受可重查，接受后成功/0xA不随补片改变。

| 字段 | 记录 |
|---|---|
| 规范关系 | Pending结果：直接规定；观察/预留边界：受规范约束，机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | §5.4.3决定已有/缺失片段的结果；空间不足推迟正式接受及固定观察时刻是模型选择。 |
| 规范依据／无直接依据说明 | §5.4.3 p59、Table12；沿用[ADR-D05](#adr-d05)的Pending优先解释。 |
| 关联要求／条目 | REQ-READFORWARD；[ADR-D05](#adr-d05)；[ADR-LOOP-P08](#adr-loop-p08)；[ADR-LOOP-L06](#adr-loop-l06) |
| 选择理由 | 不能从旧Cache/NAND补出本应0xA的结果，也不能接受后改成后来才到的数据。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [ReadPath::observe](../src/runtime/read_path.cpp#L36)；[ReadSubmission ReadPath::immediate](../src/runtime/read_path.cpp#L58)；[bool ReadPath::resolve_terminal](../src/runtime/read_path.cpp#L121) |
| 验证测试与覆盖 | 已固定Pending/0xA不会被后来补片改变；满TX重查及发射前重检。<br>[Device：pending_frozen](../CMakeLists.txt#L130)；[Device：pending_recheck](../CMakeLists.txt#L132)；[Device：pending_tx_full](../CMakeLists.txt#L130)；[生命周期：pending_fixed_missing](../CMakeLists.txt#L140) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 成功需64B结果槽；0xA可先固定元数据、等待有限TX条目；有序组合仍不支持。 |

### Global Host、逻辑工作与反馈恢复

<a id="adr-loop-h01"></a>

#### ADR-LOOP-H01｜有限Global范围先按字节地址交织，再拆为local逻辑片段；多个Global条目可共同补齐DLU。

| 字段 | 记录 |
|---|---|
| 规范关系 | 映射受规范直接公式约束；范围驱动/汇总机制未指定 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 批量拆分采用 [ADR-A03](#adr-a03) 的交织公式；Host 允许跨 Global 条目补齐 DLU，不能逐条等待 Global 完成后才发送后续补片。 |
| 规范依据／无直接依据说明 | §4.6图3 p22–23，§5.4.1完整4KiB组装；Global文件条目不是规范命令。 |
| 关联要求／条目 | REQ-ADDR；[ADR-A02](#adr-a02)；[ADR-A03](#adr-a03)；[ADR-LOOP-H02](#adr-loop-h02) |
| 选择理由 | 16channel/64B交织时单条Global4KiB只贡献各channel一部分，必须允许后续条目补齐。 |
| 模型假设／参数依据 | global base仅在interleaver去除一次；内部local保持零起点字节偏移。 |
| 实现位置（函数／成员） | [GlobalRequestSplitter::fragment](../src/host/global_request_splitter.cpp#L3)；[GlobalRequestSplitter::split](../src/host/global_request_splitter.cpp#L9)；[void HostDriver::load](../src/host/host_driver.cpp#L81) |
| 验证测试与覆盖 | 交织正反向和范围拆分、16channel跨Global补齐、正常端到端。<br>[Host：splitter](../CMakeLists.txt#L120)；[Host：sixteen_channels](../CMakeLists.txt#L120)；[Host：normal](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 沿用扁平逻辑布局和静态配置；不新增FIOSA/CSR事务。 |

<a id="adr-loop-h02"></a>

#### ADR-LOOP-H02｜默认64B写命令；只有显式burst模式才合成完整同local DLU写事务，并保持各Global子工作归属。

| 字段 | 记录 |
|---|---|
| 规范关系 | 命令/页条件受规范约束；默认拆分和聚合为Host策略 |
| 条目性质 | 可配置策略；实现机制 |
| 分类边界说明 | 规范粒度与支持能力是约束；选择64B还是burst会改变MOCS消耗，是显式实验变量。 |
| 规范依据／无直接依据说明 | §5.3.1、§5.4.1和[ADR-H01](#adr-h01)、[ADR-H02](#adr-h02)的已支持写burst子集。 |
| 关联要求／条目 | REQ-ADDR；REQ-ASSEMBLY；[ADR-H01](#adr-h01)；[ADR-H02](#adr-h02)；[ADR-LOOP-H01](#adr-loop-h01)；[ADR-LOOP-H19](#adr-loop-h19) |
| 选择理由 | 避免自动组包悄悄改变命令压力；跨Global来源仍可共享一个合法local整页事务。 |
| 模型假设／参数依据 | WriteCommandMode::Fragment64默认；Burst4096显式；整页64个片段。 |
| 实现位置（函数／成员） | [void HostDriver::validate](../src/host/host_driver.cpp#L26)；[void HostDriver::load](../src/host/host_driver.cpp#L81)；[LocalTransaction tx](../src/host/host_driver.cpp#L221) |
| 验证测试与覆盖 | 64B/burst数量差异、跨Global归属和不完整burst拒绝。<br>[Host：burst_compare](../CMakeLists.txt#L120)；[Host：burst_cross_global](../CMakeLists.txt#L120)；[Host：burst_validation](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | burst缺完整local覆盖时报BURST-INCOMPLETE，不补零、不自动读burst。 |

<a id="adr-loop-h03"></a>

#### ADR-LOOP-H03｜输入中的额外Write完成依赖直接拒绝；支持Read等待Global最终成功。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设 |
| 分类边界说明 | LIMIT-WRITE-001是当前支持范围；不等于规范禁止Host组织依赖。 |
| 规范依据／无直接依据说明 | 无直接OCP限制；关联完整DLU组装、顺序和有限MOCS前进性。 |
| 关联要求／条目 | REQ-SCOPE；REQ-MODEL；[ADR-LOOP-H01](#adr-loop-h01)；[ADR-LOOP-H19](#adr-loop-h19) |
| 选择理由 | 避免后续写提供前序DLU缺片、却又等待前序完成的循环；合法写序仍由既有规则执行。 |
| 模型假设／参数依据 | READ after可有多个Global依赖；拒绝未知ID及环。 |
| 实现位置（函数／成员） | [void HostDriver::validate](../src/host/host_driver.cpp#L26)；[bool HostDriver::dependencies_ready](../src/host/host_driver.cpp#L149)；[void HostDriver::advance()](../src/host/host_driver.cpp#L421) |
| 验证测试与覆盖 | 写依赖拒绝、16channel补齐、依赖最终失败的读。<br>[Host：input_validation](../CMakeLists.txt#L120)；[Host：sixteen_channels](../CMakeLists.txt#L120)；[Host：persistent_failure](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不对任意跨读写依赖图提供静态死锁证明。 |

<a id="adr-loop-h04"></a>

#### ADR-LOOP-H04｜content标识不可变逻辑内容；同块内容不A→B→A回用，同内容同64B片段不能重复出现在输入。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设；实现保障 |
| 分类边界说明 | content是Host工作身份，不是PEC、设备epoch或线上字段；输入去重不替代Device overlap。 |
| 规范依据／无直接依据说明 | 无直接规范要求；服务符号数据保留与逻辑工作归并。 |
| 关联要求／条目 | REQ-MODEL；REQ-SCOPE；[ADR-M01](#adr-m01)；[ADR-W04](#adr-w04)；[ADR-LOOP-H13](#adr-loop-h13) |
| 选择理由 | 使重试与新内容可区分，避免同一逻辑片段同时归属多个普通写计划。 |
| 模型假设／参数依据 | content作用域为逻辑块；新内容使用新标识；同块版本依声明次序运行。 |
| 实现位置（函数／成员） | [struct ContentKey](../include/hbf_eventlab/host/workload.hpp#L18)；[LIMIT-CONTENT-001](../src/host/host_driver.cpp#L58)；[LIMIT-CONTENT-002](../src/host/host_driver.cpp#L62) |
| 验证测试与覆盖 | 内容ID回用/重复片段拒绝及连续版本隔离。<br>[Host：content_validation](../CMakeLists.txt#L120)；[Host：versions](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | LIMIT-CONTENT-001/002是能力边界，不能描述成规范定义的错误。 |

<a id="adr-loop-h05"></a>

#### ADR-LOOP-H05｜Host按channel维护待发送集合，已Taken事务移出集合；补齐打开DLU优先，并限制打开数量。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定（Host发送策略） |
| 条目性质 | 可配置策略；实现机制 |
| 分类边界说明 | 策略必须通过固定写序/版本/恢复合法性门控；上限与优先级本身不由规范规定。 |
| 规范依据／无直接依据说明 | 关联§5.4.1完整DLU、Table13写序与MOCS/Pending约束。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-H06](#adr-loop-h06)；[ADR-LOOP-H09](#adr-loop-h09)；[ADR-LOOP-H02](#adr-loop-h02) |
| 选择理由 | 防止源每64B等待completion而凑不齐页，并允许合法补片越过受阻工作。 |
| 模型假设／参数依据 | 默认每channel2个打开DLU；名额从首个Taken持到已移交命令最终响应全部返回。 |
| 实现位置（函数／成员） | [bool HostDriver::eligible](../src/host/host_driver.cpp#L159)；[void HostDriver::pump](../src/host/host_driver.cpp#L201)；[class FillOpenDluPolicy](../include/hbf_eventlab/policies/host_send_policy.hpp#L6) |
| 验证测试与覆盖 | 打开DLU上限、补片优先、MOCS不足可超时而非伪造进展。<br>[Host：open_limit](../CMakeLists.txt#L120)；[Host：fill_open](../CMakeLists.txt#L120)；[Host：mocs_one](../CMakeLists.txt#L120)；[Host：mocs_partial](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | Host打开名额不是Device Pending数；小MOCS+64B模式仍可能真实超时。 |

<a id="adr-loop-h06"></a>

#### ADR-LOOP-H06｜Host发送策略只选择已过滤合法候选，不能直接修改设备或跳过依赖；默认选择确定。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 可配置策略；实现保障 |
| 分类边界说明 | 规范相关写序/内容条件固定；IHostSendPolicy的候选选择是研究入口。 |
| 规范依据／无直接依据说明 | §5.4.1/§11.5.2.6；当前恢复门控还受项目解释约束。 |
| 关联要求／条目 | REQ-MODEL；REQ-WRITEORDER；[ADR-LOOP-H05](#adr-loop-h05)；[ADR-LOOP-H03](#adr-loop-h03) |
| 选择理由 | 允许替换优先级实验，同时避免策略输出任意请求绕过正确性。 |
| 模型假设／参数依据 | 默认先补打开DLU，再replay，再按稳定SendUnitId平局。 |
| 实现位置（函数／成员） | [class IHostSendPolicy](../include/hbf_eventlab/interfaces/host_send_policy.hpp#L5)；[select(](../include/hbf_eventlab/policies/host_send_policy.hpp#L8)；[Host policy selected a noncandidate](../src/host/host_driver.cpp#L217) |
| 验证测试与覆盖 | 策略只能选择候选；补齐优先策略的实际次序。<br>[Host：policy_guard](../CMakeLists.txt#L120)；[Host：fill_open](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 有限候选扫描未针对百万请求优化。 |

<a id="adr-loop-h07"></a>

#### ADR-LOOP-H07｜Global读提示提供AllRegular与每范围每channel尾Batch两种配置；尾B不越过同范围未接受前序。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受规范约束，机制未指定 |
| 条目性质 | 可配置策略 |
| 分类边界说明 | §5.3.2约束提示含义，不规定一个Global软件范围怎样分配R/B。 |
| 规范依据／无直接依据说明 | §5.3.2；沿用[ADR-LOOP-W02](#adr-loop-w02)的Batch解释。 |
| 关联要求／条目 | REQ-READDISPATCH；[ADR-LOOP-W01](#adr-loop-w01)；[ADR-LOOP-W02](#adr-loop-w02)；[ADR-LOOP-H01](#adr-loop-h01) |
| 选择理由 | 形成可复现的Host调度边界，又不把Global范围强加成设备完成屏障。 |
| 模型假设／参数依据 | TailBatch默认；--regular选AllRegular；组号作用域仍为channel。 |
| 实现位置（函数／成员） | [ReadHintMode::TailBatch](../src/host/host_driver.cpp#L108)；[u.hint==ReadHint::Batch](../src/host/host_driver.cpp#L172) |
| 验证测试与覆盖 | 两种hint生成和有序读；尾B由接受进度门控。<br>[Host：hints](../CMakeLists.txt#L120)；[Host：normal](../CMakeLists.txt#L120)；[Host：ordered_reads](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 当前是枚举配置，不是独立IReadHintPolicy插件；若用户策略乱序，尾B仍受接受门控。 |

<a id="adr-loop-h08"></a>

#### ADR-LOOP-H08｜Host区分Global工作、64B逻辑片段、发送单元、命令尝试与恢复任务身份。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 规范定义本地command响应；本条主要记录模拟器上层工作归属，不是新增线上协议。 |
| 规范依据／无直接依据说明 | 关联§5.4.1一次响应；GlobalRequestId/LogicalWorkId/SendUnitId/RecoveryId均项目内部。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-H09](#adr-loop-h09)；[ADR-LOOP-H19](#adr-loop-h19) |
| 选择理由 | 一次尝试失败不必是逻辑最终失败；burst合并和重试不能丢失原Global归属。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [using GlobalRequestId](../include/hbf_eventlab/common/types.hpp#L20)；[struct Unit {](../include/hbf_eventlab/host/host_driver.hpp#L55)；[void HostDriver::finish_work](../src/host/host_driver.cpp#L273) |
| 验证测试与覆盖 | 跨Global burst归属、恢复尝试与历史成功分开。<br>[Host：burst_cross_global](../CMakeLists.txt#L120)；[Host：recovery_once](../CMakeLists.txt#L120)；[Host：prior_success_retained](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 保留有限逻辑工作历史；对象内存大小不作为硬件成本。 |

<a id="adr-loop-h09"></a>

#### ADR-LOOP-H09｜Host从Taken到最终反馈消费持续跟踪同一尝试，不在Link/RX重试时重新发送。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定（源端所有权协议） |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 完成语义有规范背景；Host与模拟传输端的Take契约、索引和ID是工程组织。 |
| 规范依据／无直接依据说明 | 关联§5.4.1响应；不是AXI新增的确认消息。 |
| 关联要求／条目 | REQ-MODEL；REQ-RESPONSE；[ADR-LOOP-L01](#adr-loop-l01)；[ADR-LOOP-L07](#adr-loop-l07)；[ADR-LOOP-P09](#adr-loop-p09) |
| 选择理由 | 比MOCS更早覆盖在途请求，保证故障排空不会漏掉尚未正式接受的事务。 |
| 模型假设／参数依据 | 按逻辑块unsettled_索引包含已移交读写；NotTaken没有最终completion。 |
| 实现位置（函数／成员） | [void HostDriver::pump](../src/host/host_driver.cpp#L201)；[void HostDriver::receive](../src/host/host_driver.cpp#L304)；[std::map<CommandId,Attempt> attempts_](../include/hbf_eventlab/host/host_driver.hpp#L131) |
| 验证测试与覆盖 | 旧Link/Ingress事务迟到、重复移交防护。<br>[Host：delayed_old_attempts](../CMakeLists.txt#L120)；[Host：ingress_old_attempts](../CMakeLists.txt#L120)；[Device：duplicate_handoff](../CMakeLists.txt#L131) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不实现通用在途撤回；缺失响应只能诊断Stalled/LimitReached。 |

<a id="adr-loop-h10"></a>

#### ADR-LOOP-H10｜正式接受通知与最终响应分开；接受通知仅推进Host补片进度和replay计数。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定（模型公开进度接口） |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | AcceptedNotice是模拟Host观察边界，不是新命令、AXI字段或completion。错误命令也会发出此通知，因此它不能证明片段已进入Assembly。 |
| 规范依据／无直接依据说明 | 关联§5.4.1/§11.5.2.6前页发送规则；代码沿用“前页全部片段正式接受”边界。 |
| 关联要求／条目 | REQ-MODEL；[ADR-W06](#adr-w06)；[ADR-LOOP-H09](#adr-loop-h09)；[ADR-LOOP-H15](#adr-loop-h15) |
| 选择理由 | Host无需读取Device私有队列即可观察前页全部命令已正式接受，以及本遍page0尝试开始；后续错误通过[ADR-LOOP-H20](#adr-loop-h20)修正暂定进度。 |
| 模型假设／参数依据 | 提供observer时异步接受通知先于一次最终响应；进度不释放credit。 |
| 实现位置（函数／成员） | [struct AcceptedNotice](../include/hbf_eventlab/interfaces/host_port.hpp#L18)；[if(t.acceptance)](../src/runtime/device_driver.cpp#L179)；[void HostDriver::accepted](../src/host/host_driver.cpp#L261) |
| 验证测试与覆盖 | 恢复快照含接受进度、正常写的前页接受门控和MOCS等待。<br>[Host：recovery_snapshot](../CMakeLists.txt#L120)；[Host：normal](../CMakeLists.txt#L120)；[Host：mocs_partial](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | Host前页accepted位图既不是介质program游标，也不是Device的received bitmap。后继可能在前页错误反馈前已经发出，须按[ADR-LOOP-H24](#adr-loop-h24)处理。 |

<a id="adr-loop-h11"></a>

#### ADR-LOOP-H11｜Host从0x7与自己的原命令地址定位失败页；0x6/info2可先到，但不能猜根因页。

| 字段 | 记录 |
|---|---|
| 规范关系 | 错误含义：直接规定；Host关联等待机制未指定 |
| 条目性质 | 行为要求；实现机制 |
| 分类边界说明 | 规范错误码不是策略；如何归并64条响应和等待真正根因是Host实现。 |
| 规范依据／无直接依据说明 | Table13 p61；§11.5.2.6–7 p122。 |
| 关联要求／条目 | REQ-REPLAY；[ADR-R01](#adr-r01)；[ADR-R05](#adr-r05)；[ADR-LOOP-H12](#adr-loop-h12) |
| 选择理由 | 后继页错误先到时也能冻结旧计划，却不把page3误作恢复终点。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [const bool recoverable=](../src/host/host_driver.cpp#L321)；[const auto root=](../src/host/host_driver.cpp#L323)；[RecoveryTask& RecoveryManager::notice](../src/host/recovery_manager.cpp#L3) |
| 验证测试与覆盖 | 0x6/info2先到仍等0x7根因；单次失败自动恢复。<br>[Host：follower_before_root](../CMakeLists.txt#L120)；[Host：recovery_once](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 如果根因始终不来，保留WaitingForProgramFailRoot，不能从内部ReplayState补答案。 |

<a id="adr-loop-h12"></a>

#### ADR-LOOP-H12｜每遍replay前冻结同块旧已移交集合，等其全部最终归属；新读不扩大集合，其他块不需清零。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受选定恢复解释约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 正确恢复普通逻辑写有规范约束；每遍有限排空屏障是保守实现，非规范规定的epoch机制。 |
| 规范依据／无直接依据说明 | §5.7、§11.5.2.7与[ADR-LOOP-H11](#adr-loop-h11)、[ADR-LOOP-H14](#adr-loop-h14)；旧事务不得误入新组装。 |
| 关联要求／条目 | REQ-REPLAY；REQ-MODEL；[ADR-LOOP-H09](#adr-loop-h09)；[ADR-LOOP-H11](#adr-loop-h11)；[ADR-LOOP-H14](#adr-loop-h14) |
| 选择理由 | 覆盖正式接受之外的迟到Link/Ingress片段；只等有限旧集合，避免新读不断加入而饿死恢复。 |
| 模型假设／参数依据 | 包含冻结时已移交旧读和旧写；首次恢复与再次失败每遍都重新取集合。 |
| 实现位置（函数／成员） | [recovery_.notice](../src/host/host_driver.cpp#L324)；[t.failed_pass=pass](../src/host/recovery_manager.cpp#L12)；[void RecoveryManager::settled](../src/host/recovery_manager.cpp#L16)；[bool HostDriver::advance_recovery](../src/host/host_driver.cpp#L383) |
| 验证测试与覆盖 | 首遍有旧Link/Ingress，第二遍有迟到旧读，必须取得最终归属后再开始。<br>[Host：delayed_old_attempts](../CMakeLists.txt#L120)；[Host：ingress_old_attempts](../CMakeLists.txt#L120)；[Host：late_read_second_pass](../CMakeLists.txt#L120)；[Host：recovery_snapshot](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 当前replay按页等待，第二次失败不存在预发后继replay写页；测试用旧读/旧响应验证同一屏障。 |

<a id="adr-loop-h13"></a>

#### ADR-LOOP-H13｜Host有限保留区按内容版本保存原Global地址和token；完整恢复前缀通过核验后固定，恢复结束才解固定。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受恢复内容要求约束，机制未指定；容量/FIFO为模型选择 |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 重写应使用同次内容；数据结构、逐出策略与深度并非规范给定。 |
| 规范依据／无直接依据说明 | §5.7/§11.5.2.7的Host恢复；符号数据沿用[ADR-M01](#adr-m01)。 |
| 关联要求／条目 | REQ-REPLAY；REQ-MODEL；[ADR-LOOP-H04](#adr-loop-h04)；[ADR-LOOP-H11](#adr-loop-h11)；[ADR-LOOP-H17](#adr-loop-h17) |
| 选择理由 | 不能读取设备NAND或用新内容覆盖旧恢复数据；原Global地址还能正确跨交织重放。 |
| 模型假设／参数依据 | 默认65536片段；FIFO逐出未固定项；全固定时不保存新数据但不阻断普通写。 |
| 实现位置（函数／成员） | [void WriteRetentionStore::save](../src/host/retention_store.cpp#L4)；[bool WriteRetentionStore::pin_prefix](../src/host/retention_store.cpp#L19)；[void HostDriver::make_replay](../src/host/host_driver.cpp#L370) |
| 验证测试与覆盖 | 保留区固定/逐出、内容版本、数据缺失和此前成功页参与恢复。<br>[Host：retention_pins](../CMakeLists.txt#L120)；[Host：versions](../CMakeLists.txt#L120)；[Host：missing_retention](../CMakeLists.txt#L120)；[Host：prior_success_retained](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 数据可能因容量不足缺失而明确失败；保留算法当前非插件；DataToken不是真字节。 |

<a id="adr-loop-h14"></a>

#### ADR-LOOP-H14｜Host 合并重复 replay，并保留最远未完成恢复终点。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受原恢复范围项目解释约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 原设备侧max终点已有实现；新增的是Host独立记录，不共享设备ReplayState。 |
| 规范依据／无直接依据说明 | 恢复终点取失败页沿用[ADR-R02](#adr-r02)的章节差异解释；合并原则沿用[ADR-R03](#adr-r03)、[ADR-R04](#adr-r04)。 |
| 关联要求／条目 | REQ-REPLAY；[ADR-R02](#adr-r02)；[ADR-R03](#adr-r03)；[ADR-LOOP-H12](#adr-loop-h12)；[ADR-LOOP-H15](#adr-loop-h15) |
| 选择理由 | 64个0x7不能启动64次replay；page2恢复期间page1再失败不能丢掉page2义务。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [RecoveryTask& RecoveryManager::notice](../src/host/recovery_manager.cpp#L3)；[bool HostDriver::advance_recovery](../src/host/host_driver.cpp#L383) |
| 验证测试与覆盖 | 再次失败保留最远终点、burst与多响应合并。<br>[Host：replay_merge](../CMakeLists.txt#L120)；[Host：burst_recovery](../CMakeLists.txt#L120)；[Host：recovery_snapshot](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 任务完成后新的普通写故障可以创建新任务；不能误解为逻辑块终身共用一次预算。 |

<a id="adr-loop-h15"></a>

#### ADR-LOOP-H15｜同一恢复任务默认最多3遍，首条page0正式接受时计本遍一次；合并失败不重置预算。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 可配置策略；实现保障 |
| 分类边界说明 | 恢复次数与具体提交计数点是工程保护，不是OCP规定重试三次。 |
| 规范依据／无直接依据说明 | 规范要求Host恢复但未给本项目默认3遍；参数随模型公开。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-H10](#adr-loop-h10)；[ADR-LOOP-H14](#adr-loop-h14)；[ADR-LOOP-H17](#adr-loop-h17) |
| 选择理由 | 既不因64条反馈多计，也不因重试嵌套无限重置；移交但未接受不算已开始的一遍。 |
| 模型假设／参数依据 | max_replay_passes默认3、0禁用；普通初始写不计。 |
| 实现位置（函数／成员） | [bool RecoveryManager::begin_pass](../src/host/recovery_manager.cpp#L24)；[bool RecoveryManager::accepted](../src/host/recovery_manager.cpp#L30)；[void HostDriver::accepted](../src/host/host_driver.cpp#L261) |
| 验证测试与覆盖 | 持续失败预算耗尽、零预算、正式接受时计数。<br>[Host：persistent_failure](../CMakeLists.txt#L120)；[Host：zero_budget](../CMakeLists.txt#L120)；[Host：recovery_snapshot](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不是可靠性最优值，没有产品校准依据。 |

<a id="adr-loop-h16"></a>

#### ADR-LOOP-H16｜当前replay逐页推进，每页补齐后等全部成功响应；终点完成后重发此前失败的后继普通页。

| 字段 | 记录 |
|---|---|
| 规范关系 | 写序受规范约束；逐页等待是保守Host机制 |
| 条目性质 | 实现机制 |
| 分类边界说明 | 不逐64B等待以免无法收齐；整页等待强于规范最小接受边界，是明确性能简化。 |
| 规范依据／无直接依据说明 | §5.4.1/§11.5.2.6及既定replay范围；规范不要求该Host算法。 |
| 关联要求／条目 | REQ-WRITEORDER；REQ-REPLAY；[ADR-LOOP-H11](#adr-loop-h11)；[ADR-LOOP-H12](#adr-loop-h12)；[ADR-LOOP-H15](#adr-loop-h15) |
| 选择理由 | 以较小恢复并行度换取明确的页进度与排空边界；旧失败命令不复活，后继重发分配新ID。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [void HostDriver::make_replay](../src/host/host_driver.cpp#L370)；[bool HostDriver::advance_recovery](../src/host/host_driver.cpp#L383) |
| 验证测试与覆盖 | 自动重放前缀、再次失败合并、后继页继续及burst恢复。<br>[Host：recovery_once](../CMakeLists.txt#L120)；[Host：replay_merge](../CMakeLists.txt#L120)；[Host：burst_recovery](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | Device在终点program成功时解除自身限制，Host确认要等终点全部响应；不是同一时刻。 |

<a id="adr-loop-h17"></a>

#### ADR-LOOP-H17｜数据不足/预算耗尽结束相关逻辑任务，继续排空旧命令；不清空设备恢复义务、不自动退役。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定（Host失败归属和退出策略） |
| 条目性质 | 实现机制；模型／输入假设 |
| 分类边界说明 | 具体逻辑任务失败、依赖取消与终止方式不是新OCP状态；Device已有义务不被宿主退出清除。 |
| 规范依据／无直接依据说明 | 关联规范恢复和容量退役语义；没有依据把“Host不再重试”等同永久容量坏。 |
| 关联要求／条目 | REQ-MODEL；REQ-SCOPE；[ADR-LOOP-H13](#adr-loop-h13)；[ADR-LOOP-H15](#adr-loop-h15)；[ADR-LOOP-H19](#adr-loop-h19) |
| 选择理由 | 持续失败也能有界结束；其他块继续成功，历史成功不追溯改写。 |
| 模型假设／参数依据 | MissingRecoveryData / RecoveryBudgetExhausted / DependencyFailed为Host诊断；同块后续内容可标PriorContentFailed。 |
| 实现位置（函数／成员） | [void HostDriver::abort_version](../src/host/host_driver.cpp#L293)；[void HostDriver::finish_work](../src/host/host_driver.cpp#L273)；[void HostDriver::advance()](../src/host/host_driver.cpp#L421) |
| 验证测试与覆盖 | 缺数据/持续失败有限终止，同块新内容取消，历史成功不回写失败。<br>[Host：missing_retention](../CMakeLists.txt#L120)；[Host：persistent_failure](../CMakeLists.txt#L120)；[Host：abort_new_content](../CMakeLists.txt#L120)；[Host：prior_success_retained](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 无自动refresh/数据生成补洞；已接受旧操作不能只从Host删除。 |

<a id="adr-loop-h18"></a>

#### ADR-LOOP-H18｜恢复期间普通读照常按Device结果结束；显式依赖成功的读才由Host推迟，不自动循环重试0x2。

| 字段 | 记录 |
|---|---|
| 规范关系 | 0x2含义受规范约束；Host发送/重试策略无直接规定 |
| 条目性质 | 可配置策略（输入依赖）；实现机制 |
| 分类边界说明 | [ADR-D06](#adr-d06)的恢复期0x2是产品选择；这里不重新定义设备读状态。 |
| 规范依据／无直接依据说明 | §11.5.1.2、§5.4.3及原Pending优先解释。 |
| 关联要求／条目 | REQ-RESPONSE；[ADR-D05](#adr-d05)；[ADR-D06](#adr-d06)；[ADR-LOOP-H03](#adr-loop-h03) |
| 选择理由 | 不让错误读触发另一写恢复；普通读和“恢复后读”有可见输入区别。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [bool HostDriver::dependencies_ready](../src/host/host_driver.cpp#L149)；[const auto& rr=std::get<ReadResponse>](../src/host/host_driver.cpp#L350) |
| 验证测试与覆盖 | 恢复中普通读、第二遍排空时的读、依赖恢复成功的读。<br>[Host：read_during_recovery](../CMakeLists.txt#L120)；[Host：late_read_second_pass](../CMakeLists.txt#L120)；[Host：recovery_once](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 自动read retry尚未策略化；有序Pending组合仍单独报能力限制。 |

<a id="adr-loop-h19"></a>

#### ADR-LOOP-H19｜Global条目等所有逻辑片段最终归属后结束；分别统计尝试错误与恢复后逻辑成功。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 规范只约束实际本地command；Global成功、失败及统计定义是模拟器上层语义。 |
| 规范依据／无直接依据说明 | 关联§5.4.1一次响应；Global汇总不合并或取消已经接受的协议命令。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-H08](#adr-loop-h08)；[ADR-LOOP-H17](#adr-loop-h17)；[ADR-LOOP-F01](#adr-loop-f01) |
| 选择理由 | 避免第一个子错误就丢弃其他outstanding，也不把可恢复尝试错误直接计为最终失败。 |
| 模型假设／参数依据 | remaining计64B逻辑片段；命令/响应计实际64B或burst事务。`pending_limit_errors`/`assembly_timeout_errors`计错误响应条数，`timeout_retry_rounds`计整页重试准备轮数；重发命令在Taken时计数，未来补片首次发送不计重传。replay中的资源重试同时属于`replay_commands`，这些维度不可直接相加。 |
| 实现位置（函数／成员） | [void HostDriver::finish_work](../src/host/host_driver.cpp#L273)；[void HostDriver::receive](../src/host/host_driver.cpp#L304)；[struct WorkResult](../include/hbf_eventlab/host/workload.hpp#L31) |
| 验证测试与覆盖 | 恢复后成功、最终失败、内容不匹配和逻辑片段汇总；[timeout_late_fragments](../tests/host_retry.cpp#L140)验证64条超时响应只驱动一轮重发，[timeout_release](../tests/host_retry.cpp#L156)区分未来补片和真正重传。<br>[Host：recovery_once](../CMakeLists.txt#L120)；[Host：persistent_failure](../CMakeLists.txt#L120)；[Host：data_mismatch](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不同内容之间未实现任意读快照语义；不能从文件行顺序推断完成依赖。 |

<a id="adr-loop-h20"></a>

#### ADR-LOOP-H20｜Host接受bitmap为暂定发送进度；错误按失败范围撤销，页级修复状态独立于Device组装位图。

| 字段 | 记录 |
|---|---|
| 规范关系 | 写序与响应受规范约束；进度修正机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 本条记录Host的发送控制机制。`AcceptedNotice`不额外承诺数据有效；Device的received bitmap和接收/program游标仍是各自状态权威。 |
| 规范依据／无直接依据说明 | 关联Table13、§11.5.2.4–6；Host bitmap及公开接受通知并非规范接口，错误响应不能被解释成成功保留数据。 |
| 关联要求／条目 | REQ-WRITEORDER；REQ-RESPONSE；[ADR-W01](#adr-w01)；[ADR-W06](#adr-w06)；[ADR-LOOP-H10](#adr-loop-h10)；[ADR-LOOP-H23](#adr-loop-h23) |
| 选择理由 | 保留正常写的接受流水，同时避免一次错误接受永久放行后继；不复制一份可能与Device漂移的Assembly状态。 |
| 模型假设／参数依据 | 普通写错误先撤销当前命令覆盖的位；timeout整页重试前，在旧尝试排空后清零整页。修复状态按内容版本、页和pass关联；普通写pass为0，block replay使用各自pass。 |
| 实现位置（函数／成员） | [Version::accepted / retries](../include/hbf_eventlab/host/host_driver.hpp#L74)；[HostDriver::accepted](../src/host/host_driver.cpp#L261)；[note_write_retry](../src/host/write_retry.cpp#L38)；[retry_eligible](../src/host/write_retry.cpp#L5)；[advance_write_retries](../src/host/write_retry.cpp#L82) |
| 验证测试与覆盖 | [pending_followers](../tests/host_retry.cpp#L90)验证错误接受曾放行后继、根因乱序及有效片段保留；[timeout_late_fragments](../tests/host_retry.cpp#L140)验证迟到旧尝试结束前不建立新一轮组装。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | bitmap仍是Host暂定记录，不能拿来查询真实Pending内容或替代Device写序检查。页级状态位于HostDriver内，不是独立策略插件。 |

<a id="adr-loop-h21"></a>

#### ADR-LOOP-H21｜Pending满只重发失败命令，保留有效补片，不以整个block排空作为重试前提。

| 字段 | 记录 |
|---|---|
| 规范关系 | 错误范围受规范约束；自动重试机制未指定 |
| 条目性质 | 实现机制；可配置策略 |
| 分类边界说明 | [ADR-H06](#adr-h06)描述Device名额规则；本条选择Host如何处理已收到的0x4，不把它改成无completion的背压。 |
| 规范依据／无直接依据说明 | §11.5.2.4及Table13约束Pending名额和已有DLU补片；命令重试的具体组织由Host实现。 |
| 关联要求／条目 | REQ-PENDING；REQ-OVERLAP；[ADR-H06](#adr-h06)；[ADR-LOOP-H09](#adr-loop-h09)；[ADR-LOOP-H20](#adr-loop-h20)；[ADR-LOOP-H22](#adr-loop-h22) |
| 选择理由 | 0x4的迟到反馈不表示整页都被丢弃；其他片段可能已有效组装。整页盲重发会造成overlap，等待整个block排空又可能阻止缺片DLU完成。 |
| 模型假设／参数依据 | 最早受影响页先修复，同pass后继的新发送暂缓；已移交命令继续取得最终归属。失败单元回队列后分配新CommandId，保留原逻辑工作和符号内容。64B与显式burst均按原发送单元重试。 |
| 实现位置（函数／成员） | [note_write_retry](../src/host/write_retry.cpp#L38)；[advance_write_retries](../src/host/write_retry.cpp#L82)；[requeue](../src/host/write_retry.cpp#L30)；[HostDriver::pump](../src/host/host_driver.cpp#L201) |
| 验证测试与覆盖 | [pending_followers](../tests/host_retry.cpp#L90)、[pending_burst](../tests/host_retry.cpp#L135)检查选择性重发和新命令身份；[real_pending](../tests/host_retry.cpp#L208)检查真实Device中Pending=1的闭环。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 有界重试不能保证持续超载时成功；自动补片不会增加配置容量。已成功接收的命令不因关联错误而被Host撤回。 |

<a id="adr-loop-h22"></a>

#### ADR-LOOP-H22｜Pending重试由容量相关完成反馈或正时间间隔timer触发，提示不保证资源可用。

| 字段 | 记录 |
|---|---|
| 规范关系 | 完成后再开DLU有依据；反馈代理及timer为无直接规定的模型选择 |
| 条目性质 | 实现机制；可配置策略；模型／输入假设 |
| 分类边界说明 | §11.5.2.4给出等待Pending写完成的方向；Host如何观察可能的容量变化、去重并兜底重查，是本模型选择，不是设备提供的空闲名额承诺。 |
| 规范依据／无直接依据说明 | [OCP v0.7.0 §11.5.2.4](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf)；1µs及无完成反馈时的定时探测没有直接数值或协议依据。 |
| 关联要求／条目 | REQ-PENDING；REQ-MODEL；[ADR-LOOP-H21](#adr-loop-h21)；[ADR-LOOP-H25](#adr-loop-h25)；[ADR-LOOP-F02](#adr-loop-f02) |
| 选择理由 | 仅使用公开反馈；保留早于迟到0x4到达的容量进展，避免漏唤醒。正间隔兜底避免缺少后续已知完成时无限停等，同时不在同Tick自旋。 |
| 模型假设／参数依据 | 同channel的Success、AssemblyTimeout、ProgramFail响应按内容/页/pass/组装轮去重，推进容量反馈版本。每次Taken保存当时版本；收到0x4后，发现新版本即可重查，否则最多等待`pending_retry_delay`（默认1000000 Tick）。 |
| 实现位置（函数／成员） | [HostDriver::receive](../src/host/host_driver.cpp#L304)；[capacity_progress_ / capacity_completions_](../include/hbf_eventlab/host/host_driver.hpp#L137)；[Unit::capacity_at_handoff](../include/hbf_eventlab/host/host_driver.hpp#L70)；[update_retry_wakeup](../src/host/write_retry.cpp#L164) |
| 验证测试与覆盖 | [pending_progress](../tests/host_retry.cpp#L124)检查先完成后错误的反馈次序和timer取消；[pending_budget](../tests/host_retry.cpp#L112)检查无有效进展时有界重试；[real_pending](../tests/host_retry.cpp#L208)检查真实资源释放。 |
| 实现状态／验证状态 | 已实现；V1（执行边界）；V0（数值校准）。 |
| 已知限制 | 容量反馈只是代理，名额可能已被其他工作占用；兜底timer可能在没有已知DLU完成时探测并再次收到0x4。它不等价于精确Pending空闲订阅，不保证最少拒绝次数。 |

<a id="adr-loop-h23"></a>

#### ADR-LOOP-H23｜timeout修复先排空同页旧尝试、检查完整数据及释放时刻，再重发整个DLU。

| 字段 | 记录 |
|---|---|
| 规范关系 | 整DLU重发被明确允许；排空与数据检查机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | [ADR-W09](#adr-w09)给出重试粒度及Device准备状态；本条是Host自动执行机制。排空当前页旧尝试与program-fail的block排空不同。 |
| 规范依据／无直接依据说明 | §11.5.2.5允许重新发送整个4KiB DLU；未规定此Host状态机、数据释放检查和重试次数。 |
| 关联要求／条目 | REQ-TIMEOUT；REQ-ASSEMBLY；[ADR-W09](#adr-w09)；[ADR-LOOP-H09](#adr-loop-h09)；[ADR-LOOP-H13](#adr-loop-h13)；[ADR-LOOP-H20](#adr-loop-h20) |
| 选择理由 | 超时清理后，旧Link/Ingress片段仍可能迟到并建立另一个不完整context；先跟踪其最终响应，防止新一轮数据混入旧context或被旧反馈清零。 |
| 模型假设／参数依据 | 同内容、页、pass中全部Handed单元结束后，检查64片覆盖；以前移交过的片段必须仍能从保留区找到，未发送片段必须到达原Global的`not_before`。满足后清空页bitmap并回队列；一次修复排空期间的多条超时响应不重复开轮。 |
| 实现位置（函数／成员） | [PageRetry::phase / timeout_rounds](../include/hbf_eventlab/host/host_driver.hpp#L48)；[note_write_retry](../src/host/write_retry.cpp#L38)；[advance_write_retries的Draining分支](../src/host/write_retry.cpp#L103)；[requeue](../src/host/write_retry.cpp#L30) |
| 验证测试与覆盖 | [timeout_late_fragments](../tests/host_retry.cpp#L140)、[timeout_future_data](../tests/host_retry.cpp#L156)、[timeout_missing_coverage](../tests/host_retry.cpp#L168)覆盖迟到、未来输入和缺失数据；[real_timeout](../tests/host_retry.cpp#L218)覆盖实际Device成功重试及持续失败。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 不发明缺片、不读取Device私有数据；保留容量不足可导致明确失败。晚到旧片段可能需要再等一次Device timeout才能排空，此等待不是新一轮Host重试。 |

<a id="adr-loop-h24"></a>

#### ADR-LOOP-H24｜只修复能关联前页资源错误的0x6/info1，根因可后到；后继重发保守地等待前页成功。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受写序约束，关联及等待机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | info1和info2的错误含义不改变；Host对info1的根因关联，以及修复时等待前页全部成功，属于实现选择。正常发送仍使用接受进度流水。 |
| 规范依据／无直接依据说明 | Table13及§11.5.2.6；规范不规定本项目的具体尝试标记或修复状态。 |
| 关联要求／条目 | REQ-WRITEORDER；[ADR-W06](#adr-w06)；[ADR-LOOP-H10](#adr-loop-h10)；[ADR-LOOP-H20](#adr-loop-h20)；[ADR-LOOP-H21](#adr-loop-h21)；[ADR-LOOP-H23](#adr-loop-h23) |
| 选择理由 | 后继错误先到时不能立即丢弃整个内容版本，也不能把任意写序错误都当作可重试的资源拒绝。等待前页真正成功使错误修复边界明确。 |
| 模型假设／参数依据 | 前页0x4/0x5只标记同内容、同pass中已Handed或已收到Order错误的具体后继尝试。若info1先到而前页仍有在途尝试则等待；前页终结后仍无可关联原因则报告`UnexplainedWriteOrderViolation`。被拒后继重发需前页全部成功；已经有效接收的后继不重复发送。 |
| 实现位置（函数／成员） | [Unit::order_retry_allowed / PageRetry::order_explained](../include/hbf_eventlab/host/host_driver.hpp#L48)；[note_write_retry](../src/host/write_retry.cpp#L38)；[advance_write_retries](../src/host/write_retry.cpp#L82)；[page_succeeded](../src/host/write_retry.cpp#L17) |
| 验证测试与覆盖 | [pending_followers](../tests/host_retry.cpp#L90)包含根因先到、后继先到、部分迟到后继合法接收三种交错；[unexplained_order](../tests/host_retry.cpp#L172)检查不能解释的错误明确失败。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 关联基于当前模型内可观察响应和具体命令尝试，并非通用故障因果推断。保守的成功屏障会降低修复期间的流水并行度，不能当成规范额外要求。 |

<a id="adr-loop-h25"></a>

#### ADR-LOOP-H25｜Pending命令重试与timeout整页重试分别设有限预算，0可禁用；耗尽或缺数据明确失败并排空。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 可配置策略；实现保障 |
| 分类边界说明 | 本条记录Host有限退出策略和默认参数；错误码来自规范，不代表规范指定三次重试、逻辑版本失败或1µs间隔。 |
| 规范依据／无直接依据说明 | 关联§11.5.2.4–5；次数、默认间隔和Host诊断无直接规定或校准依据。 |
| 关联要求／条目 | REQ-MODEL；REQ-SCOPE；[ADR-LOOP-H17](#adr-loop-h17)；[ADR-LOOP-H19](#adr-loop-h19)；[ADR-LOOP-H22](#adr-loop-h22)；[ADR-LOOP-H23](#adr-loop-h23) |
| 选择理由 | 持续超载或MOCS不足以收齐页时也能有界结束；允许实验显式关闭自动重试，对比遇错终止的基线。 |
| 模型假设／参数依据 | 默认`max_pending_retries=3`，按发送单元累计额外0x4尝试，Taken时计数；`max_timeout_retries=3`，按内容/页/pass累计额外完整组装轮数。0禁用对应功能。`pending_retry_delay=1000000 Tick`；这些预算独立于program-fail的`max_replay_passes`。 |
| 实现位置（函数／成员） | [HostConfiguration](../include/hbf_eventlab/host/workload.hpp#L41)；[HostDriver::pump](../src/host/host_driver.cpp#L201)；[advance_write_retries](../src/host/write_retry.cpp#L82)；[abort_version](../src/host/host_driver.cpp#L293)；[CLI参数](../examples/minimal_loop.cpp) |
| 验证测试与覆盖 | [pending_budget](../tests/host_retry.cpp#L112)验证耗尽/关闭；[real_timeout](../tests/host_retry.cpp#L218)验证整页预算；[timeout_future_data](../tests/host_retry.cpp#L156)验证数据不足；[partial_timeout](../tests/host.cpp#L176)保留关闭timeout重试的行为。 |
| 实现状态／验证状态 | 已实现；V1（有限失败与关闭边界）；V0（参数校准）。 |
| 已知限制 | 上限不是全workload统一次数，也不能保证重试成功。失败诊断为`PendingRetryBudgetExhausted`、`AssemblyRetryBudgetExhausted`或`WriteStatus:5 MissingRetryData`；不新增设备状态码，不自动退役或清除恢复义务。 |

<a id="adr-loop-h26"></a>

#### ADR-LOOP-H26｜页级资源修复与program-fail replay分开；replay内资源重试不另建任务或增加遍数。

| 字段 | 记录 |
|---|---|
| 规范关系 | 受既定恢复解释约束，机制未指定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 原program-fail恢复范围保持[ADR-R02](#adr-r02)的项目解释；0x4/0x5并不自动成为另一次program failure。两套进度和预算的衔接属于Host机制。 |
| 规范依据／无直接依据说明 | 关联§11.5.2.4–7及既定恢复边界；规范未指定Host内部任务/pass结构。 |
| 关联要求／条目 | REQ-REPLAY；REQ-TIMEOUT；[ADR-LOOP-H11](#adr-loop-h11)；[ADR-LOOP-H14](#adr-loop-h14)；[ADR-LOOP-H15](#adr-loop-h15)；[ADR-LOOP-H23](#adr-loop-h23)；[ADR-LOOP-H25](#adr-loop-h25) |
| 选择理由 | 防止资源重试虚增block恢复次数或丢失最远终点，也避免block恢复排空时同时建立互相冲突的页重试。 |
| 模型假设／参数依据 | replay执行期间允许当前pass内的页修复；发生program-fail或info2后，由RecoveryManager接管旧尝试排空并停用旧页修复。排空期间收到的旧资源错误不重新开启页修复。新replay使用新pass；资源重试仍属于原逻辑内容。 |
| 实现位置（函数／成员） | [HostDriver::receive](../src/host/host_driver.cpp#L304)；[advance_write_retries](../src/host/write_retry.cpp#L82)；[advance_recovery](../src/host/host_driver.cpp#L383)；[RecoveryManager::accepted](../src/host/recovery_manager.cpp#L30) |
| 验证测试与覆盖 | [retry_during_replay](../tests/host_retry.cpp#L179)分别注入Pending满及组装超时，断言仍是一项任务、一遍replay；原有[replay_merge](../tests/host.cpp#L149)继续验证真正再次program-fail的终点和遍数。 |
| 实现状态／验证状态 | 已实现；V1（已列交叉场景）。 |
| 已知限制 | 未穷举资源错误、退役、remap和多次program failure的全部交错。不能把测试通过扩大为所有故障组合的符合性证明。 |

### 运行器与停止判据

<a id="adr-loop-f01"></a>

#### ADR-LOOP-F01｜Completed须Host和Device临时活动全部排空；逻辑成功/失败另报，保留持久介质/映射/PEC/有效Cache。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 停止条件是模拟器运行契约；“排空临时状态”不要求擦除设备或取消已放弃的持久恢复限制。 |
| 规范依据／无直接依据说明 | 关联REQ-MODEL/REQ-SCOPE，无统一OCP停止命令。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-F02](#adr-loop-f02)；[ADR-LOOP-H19](#adr-loop-h19) |
| 选择理由 | 单看EventQueue返回会遗漏未发Host工作或没有事件的活动；各所有者报告而非Runner复制状态。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [RunReport SimulationRunner::run](../src/runtime/simulation_runner.cpp#L13)；[HostActivity HostDriver::activity](../src/host/host_driver.cpp#L452)；[DeviceActivity DeviceDriver::activity](../src/runtime/device_driver.cpp#L445) |
| 验证测试与覆盖 | 空输入、Host空而Device仍有活动、正常完成、失败后排空。<br>[Host：empty](../CMakeLists.txt#L120)；[Host：runner_device_activity](../CMakeLists.txt#L120)；[Host：normal](../CMakeLists.txt#L120)；[Host：persistent_failure](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | activity有重叠维度，不应求和当请求总数；部分Host摘要扫描有限历史。 |

<a id="adr-loop-f02"></a>

#### ADR-LOOP-F02｜有未来timeout/窗口/操作事件就继续跳时；仅无有效事件且仍有工作才Stalled，被动订阅不算未来进展。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | 这是离散事件前进性判定；Stalled不是OCP错误响应。 |
| 规范依据／无直接依据说明 | 项目运行契约；规范时限由现有事件落实。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-F01](#adr-loop-f01)；[ADR-LOOP-F03](#adr-loop-f03) |
| 选择理由 | 区分目前无候选与永远无动作，避免把等待介质/timeout误报停滞。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [RunReport SimulationRunner::run](../src/runtime/simulation_runner.cpp#L13)；[std::optional<Tick> EventQueue::next_time](../src/infrastructure/event_queue.cpp#L25)；[std::string HostDriver::waiting_reason](../src/host/host_driver.cpp#L463) |
| 验证测试与覆盖 | 未来组装timeout、未来release、未启动Host与无进展依赖。<br>[Host：partial_timeout](../CMakeLists.txt#L120)；[Host：delayed_release](../CMakeLists.txt#L120)；[Host：not_started](../CMakeLists.txt#L120)；[Host：stalled_dependency](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 不提供任意依赖图的因果证明；Stalled保留当前状态供诊断。 |

<a id="adr-loop-f03"></a>

#### ADR-LOOP-F03｜时间/总callback/同Tick callback/轮次数有独立限额，LimitReached保留现场；能力诊断与内部错误分开。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 可配置策略；实现保障 |
| 分类边界说明 | 预算是宿主运行保护，不是模拟硬件时延；超限不加Tick、不造completion。 |
| 规范依据／无直接依据说明 | 项目运行契约，无直接OCP数值。 |
| 关联要求／条目 | REQ-MODEL；REQ-SCOPE；[ADR-LOOP-E01](#adr-loop-e01)；[ADR-LOOP-E03](#adr-loop-e03)；[ADR-LOOP-F02](#adr-loop-f02) |
| 选择理由 | 防止无限零时延因果链，允许提高限额后继续；模型不支持与不变量破坏须可区分。 |
| 模型假设／参数依据 | Runner默认1s、总10000000 callbacks、同Tick100000 callbacks/100000轮；总数从引擎初始化累计。 |
| 实现位置（函数／成员） | [void EventQueue::check_budget](../src/infrastructure/event_queue.cpp#L33)；[RunReport SimulationRunner::run](../src/runtime/simulation_runner.cpp#L13)；[struct RunLimits](../include/hbf_eventlab/runtime/simulation_runner.hpp#L6) |
| 验证测试与覆盖 | 时间/总事件/同刻事件/轮次分别触限；模型不支持与非法策略区分。<br>[Host：time_limit](../CMakeLists.txt#L120)；[Host：event_limit](../CMakeLists.txt#L120)；[Host：same_tick_limit](../CMakeLists.txt#L120)；[Host：round_limit](../CMakeLists.txt#L120)；[Host：ordered_pending](../CMakeLists.txt#L120)；[Host：policy_guard](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 异常分类是当前C++约定：invalid_argument→ModelError，其他std异常→ImplementationError；非完整协议状态表。 |

<a id="adr-loop-f04"></a>

#### ADR-LOOP-F04｜依赖失败取消未来工作后，已无用途的release事件也取消，避免虚假时间限额退出。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现保障 |
| 分类边界说明 | 此条只清理未发送的Host release计时，不撤回已经Taken的事务。 |
| 规范依据／无直接依据说明 | 项目结束判定与事件生命周期要求。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-F01](#adr-loop-f01)；[ADR-LOOP-H17](#adr-loop-h17) |
| 选择理由 | 逻辑工作已全部有归属时，不应被取消请求的远期到达占住运行器。 |
| 模型假设／参数依据 | 本条无独立数值；沿用所关联条目的配置。 |
| 实现位置（函数／成员） | [const bool settled=std::all_of](../src/host/host_driver.cpp#L443)；[release_events_](../include/hbf_eventlab/host/host_driver.hpp#L134) |
| 验证测试与覆盖 | 被取消工作原有远期release不再引发虚假时间限额。<br>[Host：cancelled_future_release](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 已移交事件仍必须正常排空；不能泛化为任意未来事件都可删除。 |

### 参数、模型范围与输入接口

<a id="adr-loop-m01"></a>

#### ADR-LOOP-M01｜统一 Tick 和未校准的功能时延参数。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设；可配置策略（参数） |
| 分类边界说明 | 本条记录数值用途；真实介质行为不能由测试跑出Tick倒推出。 |
| 规范依据／无直接依据说明 | [ADR-M04](#adr-m04)的抽象边界；未引入可靠器件时延/硬件成本测量依据。 |
| 关联要求／条目 | REQ-MODEL；[ADR-M04](#adr-m04)；[ADR-LOOP-L04](#adr-loop-l04) |
| 选择理由 | 便于整数化有效链路服务时间和稳定事件交错；保留将来校准入口。 |
| 模型假设／参数依据 | sense=100000ps、program=500000ps、erase=1000000ps、Cache=100ps；CLI组装期限10000000ps。 |
| 实现位置（函数／成员） | [ExecutionConfiguration](../include/hbf_eventlab/architecture/execution_configuration.hpp)；[void ExecutionConfiguration::validate](../src/architecture/execution_configuration.cpp#L3)；[void DeviceDriver::execute_media](../src/runtime/device_driver.cpp#L276) |
| 验证测试与覆盖 | 验证参数合法性和执行关系；未验证默认延迟的物理真实性。<br>[Device：configuration](../CMakeLists.txt#L131)；[Device：link_latency](../CMakeLists.txt#L132) |
| 实现状态／验证状态 | 已实现于当前支持子集；V0（物理校准）；参数执行路径有测试。 |
| 已知限制 | 代码正值/因果验证不构成物理参数验证；硬件成本尚未实现。 |

<a id="adr-loop-m02"></a>

#### ADR-LOOP-M02｜缓冲、Host保留容量和演示几何独立配置，默认值不称为规范给定容量。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设 |
| 分类边界说明 | 规范存在缓冲/能力与几何限制，但RX/TX深度、演示Bank数等具体默认没有直接依据。 |
| 规范依据／无直接依据说明 | §5.2.2–5.2.3说明少量缓冲；规范能力上限仍校验；默认值为未校准的功能配置。 |
| 关联要求／条目 | REQ-MODEL；[ADR-A06](#adr-a06)；[ADR-A08](#adr-a08)；[ADR-M04](#adr-m04)；[ADR-LOOP-L06](#adr-loop-l06)；[ADR-LOOP-H13](#adr-loop-h13) |
| 选择理由 | 将研究变量和规范边界分开，使有限拥塞可重现；没有为选定数值提供硬件面积/代价最优证据。 |
| 模型假设／参数依据 | CLI 2channel×1die×1bank、8页/块、4可见块+1spare；RX16项/16384B、TX8项/512B；输入最大1048576片段。 |
| 实现位置（函数／成员） | [BufferCapacity ingress](../include/hbf_eventlab/architecture/execution_configuration.hpp)；[struct HostConfiguration](../include/hbf_eventlab/host/workload.hpp#L41)；[Geometry geometry](../examples/minimal_loop.cpp) |
| 验证测试与覆盖 | 验证配置容量、保留固定和16channel输入；未验证面积/成本和默认几何代表性。<br>[Device：configuration](../CMakeLists.txt#L131)；[Host：retention_pins](../CMakeLists.txt#L120)；[Host：sixteen_channels](../CMakeLists.txt#L120) |
| 实现状态／验证状态 | 已实现于当前支持子集；V2（执行约束）；物理依据未验证。 |
| 已知限制 | 数值选择理由仅为功能可跑通，产品合理性/敏感性待研究；C++容器内存不等于硬件预算。 |

<a id="adr-loop-m03"></a>

#### ADR-LOOP-M03｜文本输入版本化且解析器独立于核心；先完整校验再发请求，CLI报告实际配置与逻辑/尝试结果。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设；实现保障 |
| 分类边界说明 | workload文件与CLI退出码是工程接口，不是OCP命令或响应字段。 |
| 规范依据／无直接依据说明 | 项目发布/可复现性要求，REQ-SCOPE。 |
| 关联要求／条目 | REQ-SCOPE；REQ-MODEL；[ADR-LOOP-H03](#adr-loop-h03)；[ADR-LOOP-H04](#adr-loop-h04)；[ADR-LOOP-H19](#adr-loop-h19) |
| 选择理由 | 便于保留固定实验输入、识别单位/范围错误，并在替换前端时复用同一Host接口。 |
| 模型假设／参数依据 | 格式HBF_EVENTLAB_WORKLOAD 1；exit 0成功/2逻辑失败/3停滞/4限额/5模型输入/6实现错误。 |
| 实现位置（函数／成员） | [std::vector<GlobalRequest> read_workload](../src/adapters/workload_reader.cpp#L11)；[void HostDriver::validate](../src/host/host_driver.cpp#L26)；[int main(](../examples/minimal_loop.cpp#L39) |
| 验证测试与覆盖 | 格式解析/输入拒绝，正常/恢复/持续失败/缺数据的CLI结果与退出码。<br>[Host：parser](../CMakeLists.txt#L120)；[Host：input_validation](../CMakeLists.txt#L120)；[CLI：normal](../CMakeLists.txt#L110)；[CLI：recovery](../CMakeLists.txt#L112)；[CLI：persistent](../CMakeLists.txt#L114)；[CLI：missing](../CMakeLists.txt#L115) |
| 实现状态／验证状态 | 已实现于当前支持子集；V1。 |
| 已知限制 | 只支持有限内存加载输入；未实现GPU/HBM、运行时CSR/ADMIN、自动refresh或remap workload。 |

### 统计口径与观察边界

<a id="adr-stat-01"></a>

#### ADR-STAT-01｜五个生命周期时刻使用事件时间，接受后的响应延迟与端到端延迟分开。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 模型／输入假设；实现保障 |
| 分类边界说明 | 本条定义实验测量口径，不新增协议行为；RX、接受及交付的运行语义沿用既有决策。 |
| 规范依据／无直接依据说明 | OCP不指定本项目统计API或延迟汇总方法。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-L01](#adr-loop-l01)；[ADR-LOOP-L07](#adr-loop-l07)；[ADR-LOOP-H19](#adr-loop-h19) |
| 选择理由 | 将Host等待、请求传输、RX等待和接受后的服务区分，避免误把credit持有时长解释为NAND操作时延。 |
| 模型假设／参数依据 | 原Global `not_before` 为释放边界；Taken为移交；RX到达不等于接受；交付时间取Device事件，不取Host回调或导出时间。Tick=1ps沿用执行配置。分位数采用nearest-rank，成功/失败及读/写分别汇总。 |
| 实现位置（函数／成员） | [IStatisticsSink / StatisticsEvent](../include/hbf_eventlab/interfaces/statistics_sink.hpp)；[DeviceDriver::try_submit / admit / deliver](../src/runtime/device_driver.cpp)；[HostDriver::start / pump / receive](../src/host/host_driver.cpp)；[StatisticsCollector::record / summarize](../src/statistics/collector.cpp)。 |
| 验证测试与覆盖 | [statistics.cpp](../tests/statistics.cpp)：boundaries、ingress_wait、event_time、zero_and_status，验证传输公式、RX/MOCS区分、回调延后、读写同数值状态及零延迟。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 未校准性能；当前没有介质阶段、DLU组装及资源利用率的统一统计。 |

<a id="adr-stat-02"></a>

#### ADR-STAT-02｜按Global、原始逻辑工作和真实命令尝试分层关联；重试与replay保留原始归属。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | Global及其64B逻辑片段是Host工作模型；真实协议响应仍按命令产生，统计不合并响应或改变恢复。 |
| 规范依据／无直接依据说明 | 分层计数和恢复后的逻辑完成口径由项目定义，非OCP性能指标要求。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-H02](#adr-loop-h02)；[ADR-LOOP-H19](#adr-loop-h19)；[ADR-LOOP-H23](#adr-loop-h23)；[ADR-LOOP-H26](#adr-loop-h26) |
| 选择理由 | 避免把一次burst算作64次响应，把可恢复尝试错误当作最终失败，或重置释放时刻而隐藏重试成本。 |
| 模型假设／参数依据 | 一次Taken对应一个TransferAttemptId；NotTaken不创建尝试。burst可关联多个Global；replay按channel/local地址/content查回原始工作。Global在全部子工作取得最终结果时完成，时间取最后子结果；历史成功不重复完成。重试更新queued_at，原释放时间不变。 |
| 实现位置（函数／成员） | [HostDriver::load / finish_work / pump / advance_recovery](../src/host/host_driver.cpp)；[HostDriver::requeue](../src/host/write_retry.cpp)；[StatisticsCollector::write_index_ / work_ / attempts_](../include/hbf_eventlab/statistics/collector.hpp)及[record(StatAttemptLinked)](../src/statistics/collector.cpp)。 |
| 验证测试与覆盖 | [statistics.cpp](../tests/statistics.cpp)：cross_global_burst、multi_channel、pending_retry、timeout_retry、replay、mismatch；覆盖共享归属、0x4/0x5和重复program-fail、协议成功但Host符号数据不匹配。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | replay_attempts与retried_unit_attempts可交叉，不能相加；保留历史成功不表示故障后旧数据始终可读。 |

<a id="adr-stat-03"></a>

#### ADR-STAT-03｜观察与执行分离，未完成和提前取消保留明确状态，JSON使用版本化格式。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 统计存储和API属于模拟器宿主基础设施，不是设备硬件容量；不会引入新的硬件缓冲或时延。 |
| 规范依据／无直接依据说明 | 无直接规范条款；服务实验复现和观测正确性。 |
| 关联要求／条目 | REQ-MODEL；[ADR-LOOP-F01](#adr-loop-f01)；[ADR-LOOP-F03](#adr-loop-f03)；[ADR-LOOP-F04](#adr-loop-f04) |
| 选择理由 | 让不同实验使用同一口径，防止限额停止后只报告已完成请求而隐去未完成工作，也避免未释放取消产生负延迟。 |
| 模型假设／参数依据 | 默认不挂接；一对Host/Device共用一个sink，load/首次移交前挂接且覆盖其生命周期。默认collector保存精确历史，O(逻辑工作＋尝试)空间；缺失时刻为optional/JSON null，零时刻有效。JSON schema=1；逻辑失败、协议错误、未完成分开。未释放取消是failed子集，不纳入端到端分布。 |
| 实现位置（函数／成员） | [StatisticsCollector::summarize / write_json](../src/statistics/collector.cpp)；[HostDriver::set_statistics_sink](../src/host/host_driver.cpp)；[DeviceDriver::set_statistics_sink](../src/runtime/device_driver.cpp)；[CLI --stats-json](../examples/minimal_loop.cpp)。 |
| 验证测试与覆盖 | [statistics.cpp](../tests/statistics.cpp)：unfinished_resume、cancelled_future、partial_future_cancel、backpressure、observational、guards_empty、stalled；覆盖同一Global部分子工作提前取消而其余仍释放的边界。[check_statistics_cli.cmake](../tests/check_statistics_cli.cmake)解析完整及限额运行的JSON，并检查输出不覆盖输入。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 开启精确历史会增加宿主运行时间和内存，暂非有界流式聚合；自定义sink须遵守只观察契约。不包含墙钟耗时。 |

### 配置装配与参数边界

<a id="adr-cfg-01"></a>

#### ADR-CFG-01｜统一配置按默认值、文件、显式CLI覆盖装配，校验后冻结并向组件传递子配置。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；实现保障 |
| 分类边界说明 | 配置组织属于模拟器基础设施，不改变Host／Device协议状态所有权。 |
| 规范依据／无直接依据说明 | OCP不规定模拟器配置格式、覆盖顺序或代码组织。 |
| 关联要求／条目 | [ADR-A06](#adr-a06)；[ADR-H07](#adr-h07)；[ADR-LOOP-M03](#adr-loop-m03) |
| 选择理由 | 让实验无需修改源码，并避免省略的CLI默认值覆盖文件、组件各自解释同一参数。 |
| 模型假设／参数依据 | 单一INI子集；文件重复字段、未知字段及无效组合报错，CLI同键最后一次生效。运行期不热修改。 |
| 实现位置（函数／成员） | [SimulationConfiguration / ResolvedSimulationConfiguration](../include/hbf_eventlab/configuration/simulation_configuration.hpp)；[read_configuration / apply_configuration_override / resolve_configuration](../src/configuration/simulation_configuration.cpp)；[CLI装配](../examples/minimal_loop.cpp)。 |
| 验证测试与覆盖 | [configuration.cpp](../tests/configuration.cpp)：parse_override、parser_errors、unknown_fields、numeric、frozen；[CLI测试](../tests/check_configuration_cli.cmake)：precedence、duplicate、unknown、invalid_mocs。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 不支持配置继承、环境展开及运行时CSR修改；外部C++调用者须使用重组后的子配置。 |

<a id="adr-cfg-02"></a>

#### ADR-CFG-02｜Speed Grade派生有效带宽和结构上限；控制服务量及固定传播延迟单列为模型假定。

| 字段 | 记录 |
|---|---|
| 规范关系 | 档位表直接规定；模型档位与几何维度关联、事务计费机制由项目选择 |
| 条目性质 | 行为要求；模型／输入假设；实现保障 |
| 分类边界说明 | 最大模块数／堆叠层数不强迫实际取满；不把有效带宽、控制当量和真实AXI开销等同。 |
| 规范依据／无直接依据说明 | OCP v0.7.0 §5.1.2.1—§5.1.2.3：48／94／188GB/s及各档位上限。 |
| 关联要求／条目 | [ADR-LOOP-L04](#adr-loop-l04)；[ADR-LOOP-M01](#adr-loop-m01) |
| 选择理由 | 避免独立填写带宽与结构上限时构造相互矛盾的配置，同时保留可研究的链路开销模型。 |
| 模型假设／参数依据 | 一个Host channel对应一个UCIe模块，dies_per_channel对应可寻址Core堆叠层数。Grade1上限8模块／8层，Grade2／3均16／16；服务量与时延单独校验非负或正值及算术溢出。 |
| 实现位置（函数／成员） | [ucie_speed_profile()](../src/architecture/ucie_profile.cpp)；[UcieExecutionConfiguration / LinkOverheadConfiguration](../include/hbf_eventlab/architecture/execution_configuration.hpp)；[ExecutionConfiguration::validate / serialization](../src/architecture/execution_configuration.cpp)。 |
| 验证测试与覆盖 | [configuration.cpp](../tests/configuration.cpp)：grades、grade_limits、timing、buffers、zeros；[CLI测试](../tests/check_configuration_cli.cmake)：invalid_grade、buffer。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 不模拟完整UCIe或AXI；有效带宽表并不能校准介质时延和缓冲容量。 |

<a id="adr-cfg-03"></a>

#### ADR-CFG-03｜策略名称与参数统一配置；仅装配已实现策略，不忽略未知或不适用的设置。

| 字段 | 记录 |
|---|---|
| 规范关系 | 策略受规范约束；配置机制无直接规定 |
| 条目性质 | 可配置策略；实现机制；实现保障 |
| 分类边界说明 | 配置只能选择实现中存在的策略，策略选择不能绕过正确性过滤。 |
| 规范依据／无直接依据说明 | 无直接条款规定配置格式；合法候选、顺序与资源约束沿用各路径决策。 |
| 关联要求／条目 | [ADR-LOOP-R03](#adr-loop-r03)；[ADR-LOOP-H06](#adr-loop-h06) |
| 选择理由 | 使策略实验可复现，避免文件看似启用了某策略但实际仍运行默认实现。 |
| 模型假设／参数依据 | 入口支持fill_open／window／fcfs／fifo；窗口wait=0不等待，threshold=0关闭数量触发。C++自定义对象仍可注入，内部配置视图标custom。 |
| 实现位置（函数／成员） | [resolve_configuration()策略校验](../src/configuration/simulation_configuration.cpp)；[HostDriver构造函数](../src/host/host_driver.cpp)；[DeviceDriver构造函数](../src/runtime/device_driver.cpp)。 |
| 验证测试与覆盖 | [configuration.cpp](../tests/configuration.cpp)：policies、custom_policy、zeros；[CLI测试](../tests/check_configuration_cli.cmake)：policy。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | Host FCFS尚未实现；有效配置导出不替自定义C++策略推断其私有参数。 |

<a id="adr-cfg-04"></a>

#### ADR-CFG-04｜有效配置可导出并重载；Core拓扑、容量预算和派生容量使用同一组冻结参数。

| 字段 | 记录 |
|---|---|
| 规范关系 | 无直接规定 |
| 条目性质 | 实现机制；模型／输入假设；实现保障 |
| 分类边界说明 | 配置快照及派生预算用于实验复现；逻辑payload预算不等于模拟器实际内存，也不是规范硬件容量。 |
| 规范依据／无直接依据说明 | OCP不规定本项目快照格式；Core理想模型及补偿预算沿用已有模型选择。 |
| 关联要求／条目 | [ADR-C07](#adr-c07)；[ADR-LOOP-M02](#adr-loop-m02)；[ADR-STAT-03](#adr-stat-03) |
| 选择理由 | 防止日志输出固定默认值、参数只被解析却未传入运行组件，或重载时遗漏隐含默认值。 |
| 模型假设／参数依据 | zone auto和空Core路由在resolve时展开；派生量以注释导出、不可单独覆盖。Pending与每DLU 4096B组装预算耦合；RX／TX总预算和地址容量做溢出检查。输出不覆盖输入文件或另一输出。 |
| 实现位置（函数／成员） | [write_effective_configuration / write_configuration_summary](../src/configuration/simulation_configuration.cpp)；[CoreChannelTopology](../src/architecture/topology_configuration.cpp)；[DeviceDriver::capacity_初始化](../src/runtime/device_driver.cpp)；[CLI路径保护](../examples/minimal_loop.cpp)。 |
| 验证测试与覆盖 | [configuration.cpp](../tests/configuration.cpp)：topology、capacity、roundtrip、all_keys、default_equivalence；[CLI测试](../tests/check_configuration_cli.cmake)：roundtrip、collision、workload_guard、config_guard。 |
| 实现状态／验证状态 | 已实现；V1。 |
| 已知限制 | 快照仅包含运行参数，不嵌入workload、程序版本或统计；完整复现需共同保存这些材料。 |

## 5. 参数与模型假设

当前默认配置、单位与CLI/API入口统一记录在[参数说明](parameters.md)。具体选择理由保留在各ADR中，尤其是[功能时延](#adr-loop-m01)、[容量与几何](#adr-loop-m02)、[链路计费](#adr-loop-l04)、[replay预算](#adr-loop-h15)和[资源重试预算](#adr-loop-h25)。

64B、4KiB等规范粒度与模型参数分开；Core理想带宽、固定NAND时延、RX/TX深度和Host容量均不能自动成为真实产品依据。稀疏补偿默认50‰是研究策略参数，不是OCP默认值。变化参数时应同步参数说明、相关ADR和验证条件。

## 6. 测试与验证索引

全量矩阵、Debug/Release结果、CLI计数和复现命令统一记录在[验证说明](verification.md)。基础T01–T35的函数位置见[追溯键索引](verification.md#test-index)。

本文用场景函数名组织测试引用，并区分生命周期、Device、Host与CLI；实际CTest注册以[CMakeLists.txt](../CMakeLists.txt)为准。每个条目的V1/V2/V0只描述证据覆盖，不表示完整规范符合性。行号是当前定位线索，不是稳定API。

## 7. 当前支持范围与已知限制

完整列表统一记录在[支持范围与限制](scope_and_limitations.md)，包括响应子集、有序Pending、有限workload、协议/介质抽象和未校准性能。对超出支持子集的诊断不能冒充规范规定的错误响应。

Host已经能从自己的保留区自动组织replay；缺少原内容时明确失败，不从设备私有NAND状态补齐。

## 8. 代码定位与复现入口

- [架构](architecture.md)：组件职责、状态所有权和生命周期。
- [使用](usage.md)：构建、Global输入、CLI、参数及退出码。
- [验证](verification.md)：命令、预期结果与覆盖边界。
- 核心闭环位于[Host](../src/host/)、[Runtime](../src/runtime/)、[事件基础设施](../src/infrastructure/)；接口位于[include](../include/hbf_eventlab/interfaces/)。
- [CLI](../examples/minimal_loop.cpp)、[示例workload](../examples/workloads/)和[CMake](../CMakeLists.txt)是可执行入口。
