# HBF EventLab 参数

统一入口采用单个 INI 配置文件。参数按 device、address、host、execution、faults、run、output 分组；配置在创建运行组件前解析、校验并冻结。默认值目前是未经校准的产品参数，仅服务于功能验证。

## 使用与覆盖顺序

从仓库根目录运行，例如：

```powershell
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --config configs\functional.ini --dump-config effective.ini --stats-json statistics.json
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --config configs\functional.ini --mocs-supported 1024 --mocs 512 --set host.max_open_dlus_per_channel=4
```

Linux 使用相应构建目录中的 `hbf_eventlab_cli`；配置格式相同。

覆盖顺序为 **内建默认值 → 一个配置文件 → 显式 CLI 覆盖**。`--config` 在命令行中的位置不影响此顺序；多个 `--set` 或简写选项修改同一字段时，按命令行从左到右，最后一次生效。未提供的 CLI 选项不会覆盖文件值。一个文件内重复指定同一完整键会报错。

`--set section.key=value` 可以覆盖本文列出的全部输入字段。既有 `--channels`、`--mocs`、`--open` 等是同一字段的简写，见 [CLI 用法](usage.md#cli参数)。`--help` 打印用法。

```ini
[device.capabilities]
mocs_supported = 1024

[device.selected]
mocs_selected = 512

[execution.read_dispatch]
policy = window
wait_ps = 20000
threshold = 16
```

INI 支持空行、以 `#` 或 `;` 开头的整行注释、`[section]` 和 `key=value`。也可以不写小节，直接写完整的点分键。数值支持无符号十进制和 `0x` 十六进制；不支持负数、浮点数或隐含单位后缀。布尔值必须为 `true` / `false`；列表使用 `[0, 1]`。字符串可不加引号，也可使用双引号；引号内的双引号和反斜杠需用反斜杠转义。Windows 路径推荐用 `/`，或使用不带引号的原始路径值。仅支持单行值，不支持行尾注释、文件包含、环境变量展开或表达式求值。这是项目定义的 INI 子集，不是 TOML。

路径相对于**运行进程工作目录**解释，不相对于配置文件。输出目录须已经存在；统计与有效配置输出不得覆盖输入 workload、输入配置或彼此。未知字段、小节、策略，以及只读派生量被当作输入设置时，均明确报错。

## device：结构、能力与设备选定参数

### 档位与几何

| 完整配置键 | 默认值 | 含义、来源与约束 |
|---|---|---|
| `device.spec_profile` | `ocp_hbf_v0.7.0` | 当前只接受这一规范参考版本；仍仅实现规范的选定子集，不代表一致性认证。 |
| `device.ucie_speed_grade` | `1` | 选择 1/2/3，按下表派生每模块、每方向的有效带宽和结构上限。 |
| `device.geometry.host_channels` | `2` | 实际 UCIe channel／模块数，正数且不超过档位上限。 |
| `device.geometry.dies_per_channel` | `1` | 每 channel 可寻址的 Core Die 层数；当前布局接受 1/2/4/8/16，且不超过档位堆叠上限。不是 channel 数与 die 数的乘积。 |
| `device.geometry.banks_per_die` | `1` | 每 die 的 Bank 数，正数；允许小型功能几何，不表示任意数值均为产品配置。 |
| `device.geometry.pages_per_block` | `8` | 每 block 的 page 数，正数；当前功能布局的维度。 |
| `device.geometry.blocks_per_bank` | `4` | 每 bank 的可见逻辑 block 数，正数。 |
| `device.geometry.spare_blocks_per_bank` | `1` | 每 bank 额外物理备用块，0 表示没有备用块；不增加 Host 可见容量。 |

OCP HBF v0.7.0 §5.1.2.1—§5.1.2.3 给出以下档位。采用该节明确给出的有效值，不用 raw bandwidth 重新推算：

| Grade | UCIe GT/s | 有效 GB/s／模块／方向 | 最大模块数 | 最大堆叠层数 |
|---|---:|---:|---:|---:|
| 1 | 8 | 48 | 8 | 8 |
| 2 | 16 | 94 | 16 | 16 |
| 3 | 32 | 188 | 16 | 16 |

上限不强迫实际数量取最大值。选择 Grade 2、实际使用 2 个 channel 和 1 层是可接受的功能配置；Grade 1 配置 16 个 channel 会被拒绝。带宽和档位上限没有独立写入入口，避免相互矛盾的配置。各 channel 的带宽不因其他 channel 存在而被平均分摊；Core Channel 仍采用理想模型。

每 channel 可见容量由 `dies × banks × blocks × pages × 4096B` 派生，总容量再乘 channel 数。算术溢出会报错。调整 Bank 数可能同时改变总容量，实验中应另行控制资源预算和 workload 覆盖公平性。

### 能力与选定值

| 完整配置键 | 默认值 | 含义、来源与约束 |
|---|---|---|
| `device.capabilities.mocs_supported` | `256` | 每 channel 支持的 command 上限；接受 1 或 256—16384 范围内的 2 的幂。 |
| `device.selected.mocs_selected` | `256` | 每 channel 本次选定上限；同上取值范围，且不超过 Supported。 |
| `device.capabilities.max_pending_write_dlus` | `8` | 每 channel Pending 写上下文上限，必须为正；默认值为模型选择。 |
| `device.capabilities.read_burst` | `false` | 当前自动 Device 路径只支持 64B 读，设为 true 会在装配前报错。 |
| `device.capabilities.write_burst` | `true` | 是否允许一条 4KiB 写事务。能力启用不表示 Host 自动使用 burst。 |
| `device.selected.assembly_timeout_ps` | `10000000` | 组装期限，单位 ps，必须为正；默认时长为功能模型假定。 |
| `device.selected.zone_size_dlus` | `auto` | 输入 auto 时取一个 block group；显式值必须为正、为 block group 的整数倍且整除 channel DLU 数。这是当前布局约束。 |

Pending 上限与组装存储仍耦合：每 channel 配置 `Pending × 4096B` 的逻辑 payload 预算，保留至上下文终结。这不是 C++ 容器实际内存占用。

**合法性不等于前进性。** MOCS=1、关闭 write burst 仍可构造设备并执行纯读。实际 workload 含 64B 写且 Selected 小于 64 时，CLI 输出 `WRITE_CREDIT_INSUFFICIENT` 警告并继续运行；超时、重试和逻辑失败由真实执行路径产生。即使 MOCS≥64，也可能因为 credit 分散到多个未收齐 DLU 而超时，诊断不保证前进。

### 拓扑与退役预算

| 完整配置键 | 默认值 | 含义、来源与约束 |
|---|---|---|
| `device.topology.core_channels_per_ucie` | `1` | 每 UCIe channel 的 Core Channel 数，1 至 dies_per_channel。仅改变拓扑分组，当前不产生有限带宽争用。 |
| `device.topology.die_to_core_channel` | `[]` | 每个 die 对应的 Core Channel 索引；空列表按 die%Core 数派生。显式列表长度必须等于 die 数，索引不得越界；不要求每条 Core Channel 均被使用。 |
| `device.capacity_policy.max_hole_permille` | `50` | 每 channel 补偿条目预算比例，0—1000‰。50 表示 5%；以 nominal blocks 为分母向下取整，不隐式保证至少一项。无规范数值依据；已传入运行中的协调器。 |

## address：Host Global 地址

| 完整配置键 | 默认值 | 含义与约束 |
|---|---|---|
| `address.global_base` | `0` | Global 字节地址起点，64B 对齐；映射时只减去一次。 |
| `address.interleave_bytes` | `64` | channel 交织条带字节数；当前 interleaver 要求正数、64B 整数倍且整除每 channel 容量。 |

输入 range 必须落在由 base 和派生总容量确定的区间，局部地址为 channel Flash 区域内的零起点字节偏移。配置校验也检查 base+总容量溢出。

## host：发送、重试与保留

| 完整配置键 | 默认值 | 含义、范围与零值语义 |
|---|---|---|
| `host.send_policy` | `fcfs` | `fcfs`：每 channel 当前合法候选中先入队者优先，受阻者可被越过；`fill_open`：补齐已打开 DLU 优先，其次 replay，再按入队顺序。 |
| `host.write_mode` | `fragment64` | `fragment64` 或 `burst4096`；后者要求设备支持 write burst，且输入能覆盖完整 local DLU。不自动改变组包。 |
| `host.read_hint_mode` | `tail_batch` | `tail_batch` 或 `all_regular`；前者在每 Global、每 channel 的最后一条读使用 Batch。 |
| `host.max_open_dlus_per_channel` | `2` | 正数。Host 活跃写 DLU 数：从首条命令 Taken 到该 DLU 已移交命令全部收到响应；不是 Device Pending 数。 |
| `host.retained_fragments` | `65536` | Host 恢复保留区额度，单位为代表 64B 内容的符号片段；0 禁用。默认相当于 4MiB 逻辑内容，不等于程序实际内存。 |
| `host.max_logical_fragments` | `1048576` | 正数；原始逻辑片段累计输入上限。增量模式按整个 Host 生命周期累计，已经完成的输入也计入；超限批次拒绝。属于程序保护，不是当前队列容量、Device MOCS 或硬件容量。 |
| `host.recovery.max_replay_passes` | `3` | 每恢复任务最多 replay 遍数，初始普通写不计；0 禁止 replay。重复响应及合并任务不重置预算。 |
| `host.retry.max_pending_retries` | `3` | 每发送单元收到 0x4 后允许的额外尝试，初始发送不计；0 关闭。 |
| `host.retry.max_timeout_retries` | `3` | 同内容／页／pass 的额外整页组装轮数，初始组装不计；0 关闭。 |
| `host.retry.pending_delay_ps` | `1000000` | 0x4 后兜底等待间隔；启用 Pending 重试时必须为正，相关完成反馈可提前唤醒。 |

这些 Host 默认策略与预算是项目选择，不是 OCP 指定的 Host 算法。重试次数的三种计数单位不能互换。

`fcfs` 的“先来”由本次入队序号确定，稳定的 `SendUnitId` 只标识发送单元。普通请求按 `at` 释放时间、输入行及行内片段次序登记；显式 4KiB burst 等所有贡献片段释放后，按最后一个贡献片段的位置入队。依赖或资源等待、`NotTaken` 背压均保留原序号；错误尝试终结且满足现有重试门控后，重发单元重新排到队尾。新建 replay 单元同样取得新序号。规则不改变原逻辑工作的释放时间和统计分母，也不添加人为 Tick。详见[Host 行为](usage.md#host-send-policy)。

## execution：执行模型与策略

### 介质与链路

所有 `_ps` 字段均为整数皮秒；**1 Tick=1ps 固定不变**。下列时延和控制服务量为未校准模型假定。

| 完整配置键 | 默认值 | 约束与含义 |
|---|---|---|
| `execution.media_timing.sense_ps` | `100000` | NAND sense 服务时间，正数。 |
| `execution.media_timing.program_ps` | `500000` | program 服务时间，正数。 |
| `execution.media_timing.erase_ps` | `1000000` | erase 服务时间，正数。 |
| `execution.media_timing.cache_access_ps` | `100` | Cache 读取服务时间，正数。 |
| `execution.ucie.request_control_bytes` | `16` | 请求方向控制服务字节当量，正数，不是逐拍 AXI 或真实包头尺寸。 |
| `execution.ucie.response_control_bytes` | `16` | 返回方向控制服务字节当量，正数。 |
| `execution.ucie.request_fixed_latency_ps` | `100` | 请求序列化后附加固定延迟，允许 0；不延长链路带宽占用。 |
| `execution.ucie.response_fixed_latency_ps` | `100` | 返回序列化后附加固定延迟，允许 0。 |

序列化时间为 `ceil((控制服务字节 + payload 字节) × 1000 / 档位有效GBps)` Tick；请求、返回分别占用各自方向的链路。当前采用有效带宽计费，不另建逐拍 AXI。可表示范围内仍可能发生乘法／加法溢出，最大已支持事务的计费会在配置阶段检查；运行期间的事件时间加法也有溢出保护。

### 缓冲与调度

| 完整配置键 | 默认值 | 约束与含义 |
|---|---|---|
| `execution.buffers.ingress_entries` | `16` | 每 channel RX 条目数，正数。 |
| `execution.buffers.ingress_payload_bytes` | `16384` | 每 channel RX payload 总预算；须容纳最大声明支持的写事务：write_burst=true 时至少4096B，否则至少64B。 |
| `execution.buffers.egress_entries` | `8` | 每 channel TX 条目数，正数。 |
| `execution.buffers.egress_payload_bytes` | `512` | 每 channel TX payload 总预算，至少64B。无 payload 的响应仍占一个条目。 |
| `execution.read_dispatch.policy` | `window` | 当前配置入口只提供 window 策略；API 可以注入自定义实现。 |
| `execution.read_dispatch.wait_ps` | `10000` | 窗口等待时间；0 表示不等待累积。 |
| `execution.read_dispatch.threshold` | `32` | 等待 Core sense 的窗口成员数触发阈值；0 关闭数量触发，timer 和 Batch 仍可释放。 |
| `execution.nand_scheduler.policy` | `fcfs` | `fcfs`：合法候选按现有序号优先；`read_priority`：合法读优先，再按序号／ID，program与erase之间仍为FCFS。非抢占，不绕过窗口／顺序／资源限制；持续读可能使写饥饿。 |
| `execution.ucie_scheduler.policy` | `fifo` | 当前配置入口只提供返回方向 FIFO 仲裁。 |

窗口阈值高于 MOCS 不会被当作非法配置：timer／Batch 仍能推进。条目预算、payload 预算和链路服务字节分别计费，不能当作同一个硬件内存数值。

参数与策略在同一小节中指定。不支持的策略或字段明确报错，不回退为默认实现。自定义 C++ 策略由调用方负责记录其类型与参数；Device 配置视图会将相应选择标为 `custom`，这不表示窗口默认数值已应用于自定义对象。有效配置导出只描述本统一入口实际装配的内建策略。

## faults：故障输入

| 完整配置键 | 默认值 | 约束与含义 |
|---|---|---|
| `faults.channel` | `0` | 目标逻辑 channel，必须在几何内。 |
| `faults.die` | `0` | 目标逻辑 die，必须在几何内。 |
| `faults.bank` | `0` | 目标逻辑 bank，必须在几何内。 |
| `faults.block` | `0` | 目标逻辑 block，必须在可见几何内。 |
| `faults.fail_sequence` | `[]` | 页号列表，例如 `[2, 1]`，按序命中目标页时各注入一次 FailedReusable。 |
| `faults.persistent_page` | `none` | 指定页每次 program 均 FailedReusable；none 禁用。 |

页号不得超出 pages_per_block；sequence 和 persistent 不同时启用。原有 CLI 故障选项默认仍作用于逻辑 block 0，但现在可通过统一配置改变目标。故障注入器独立于 Host，Host 只看到正常响应接口。

## run 与 output

| 完整配置键 | 默认值 | 约束与含义 |
|---|---|---|
| `run.max_tick_ps` | `1000000000000` | 绝对模拟时间上限，默认1s；none 禁用该上限，0 仅允许推进到 Tick 0。不是宿主墙钟超时。 |
| `run.total_events` | `10000000` | 从事件引擎初始化起累计 callback 上限；0 不限制。 |
| `run.events_per_tick` | `100000` | 同 Tick callback 上限；0 不限制。 |
| `run.rounds_per_tick` | `100000` | 同 Tick 因果轮次上限；0 不限制。 |
| `output.trace` | `false` | 是否输出 Host／Device 事件日志。 |
| `output.statistics_json` | 空字符串 | 统一统计 JSON 路径（schema 2），包含请求延迟、Pending曲线、DLU上下文历史、Bank操作、读窗口与实际读服务；空字符串关闭记录。精确历史有宿主内存开销，详见[统计说明](usage.md#统一统计)。 |
| `output.effective_config` | 空字符串 | 有效配置 INI 导出路径；空字符串关闭文件导出。 |

这些运行限额不属于设备资源。基础 EventQueue 单独使用时默认总事件数不限；统一入口使用本表 RunLimits 作为程序级默认值。

## 有效配置与扩展

`--dump-config` 在运行前输出已经展开默认值、zone 大小和 Core 路由的配置。派生带宽、档位上限、逻辑容量和组装预算写为只读注释，主体可以再次经 `--config` 加载。重放导出文件时，如果其中的 output.effective_config 恰好指回输入文件，请用 `--dump-config another.ini` 覆盖，避免覆盖保护报错。

每次实验建议保存 workload、有效配置、统计 JSON、程序版本和运行命令。改变配置值不必重新编译。当前没有多文件继承或运行中热修改配置。

C++ 装配流程位于 [统一配置实现](../src/configuration/simulation_configuration.cpp)。运行组件接收各自的类型化子配置，不解析 INI，也不共享可变的全局配置对象。Core 拓扑与退役预算进入 StaticDeviceDescription；ExecutionConfiguration 由介质、UCIe、缓冲和读调度子配置组合。旧扁平 ExecutionConfiguration 字段已迁移，Supported／Selected 字段分别命名为 mocs_supported／mocs_selected；外部 C++ 调用者需要按新头文件调整。

固定协议粒度64B／4KiB、每 Bank 同时一项介质操作和双 Page Cache 不通过本入口任意修改；引入新的微架构模型需要相应实现和验证。新增参数时同步维护参数定义／默认值、字段绑定、校验、本文及测试。只新增一个能解析却不被任何组件使用的字段不算完成接入。

容量交互实验的扫描轴和固定条件见[实验参数](../experiments/capacity/README.md#固定条件与扫描变量)。计划JSON是实验工具的矩阵入口，由工具生成此处定义的标准配置；未引入另一套设备参数解释。每组`effective.ini`保留全部默认值和派生值。

[交织与到达间隔实验](../experiments/interleave_arrival/README.md#固定条件与矩阵)同样生成标准配置；计划中的`delta_ps`用于生成workload的`at`，不是新增设备时延参数。固定MOCS/Pending预算，扫描交织粒度和Host端释放间隔，保留实际链路、接收与组装等待。
