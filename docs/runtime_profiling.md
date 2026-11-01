# 程序性能诊断

本模块测量**模拟器程序自身的墙钟耗时**，不测量模拟中的HBF延迟，不改变Tick、事件排序、64B拆分、Host容量、重试或回收条件。默认关闭，与`--stats-json`／`--stats-events`独立。

## 启用方式

普通CLI通过统一参数入口启用：

```powershell
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --runtime-profile build\runtime.json
# 等价：--set output.runtime_profile_json=build/runtime.json
```

INI对应`[output] runtime_profile_json="build/runtime.json"`；空字符串关闭。输出不得覆盖workload、输入配置或其他输出。正常、逻辑失败及运行限额停止都可导出；进程被强制终止不会保存内存中的最终汇总。输入错误发生在运行前时，不保证生成报告。

C++调用方创建`RuntimeProfile`，在开始输入前分别调用`HostDriver::set_runtime_profile(profile)`和`DeviceDriver::set_runtime_profile(profile)`；在线路径通过`SimulationSession(resolved, sink, &profile)`或`TensorAccessBridge(resolved, regions, bridge_config, sink, &profile)`自动连接两侧。传`nullptr`关闭。对象由调用方持有，必须比Host、Device及其通知注册表／Session／Bridge活得更久；同一实例仅用于同一模拟线程。Session不自动解释输出路径或创建文件。

运行返回、没有活动计时作用域时，调用`profile.write_json(stream)`导出一次。暂停后也可读取累计快照，但不会清零；不要把多个累计快照相加。不要在事件回调内导出。

## 计时口径

使用`std::chrono::steady_clock`，输出单位为`program_wall_ns`。纳秒是表示单位，不保证计时器具有纳秒精度。每个作用域入口和出口各读取一次时钟；不为每个64B片段单独取时间，不保存逐调用历史。

| JSON区域 | 测量位置／范围 |
|---|---|
| `split` | `GlobalRequestSplitter::split()`：地址映射、Fragment构造和vector增长。 |
| `prepare` | `HostDriver::prepare()`：输入校验、依赖与内容覆盖检查，包括调用split。 |
| `register_input` | `HostDriver::register_input()`：Work／Unit／Global登记、统计观察及释放安排。 |
| `admit_input` | 增量输入准入，包括准备、剩余容量检查和登记；有限CLI的load不经过此项。 |
| `pump` | Host发送候选扫描、策略选择、构造本地事务和链路移交尝试。 |
| `receive` | Host处理一个命令响应，包括可能触发的推进和回收。 |
| `advance` | Host状态推进，包括恢复／重试及Global完成计数查询。 |
| `completion_check` | advance中读取未完成Global计数；每次advance执行一次，不遍历历史。 |
| `results_scan` | 兼容旧诊断报告保留的区域；当前Host不执行此扫描，数值为0。 |
| `reclaim` | 详细Work／Unit／Global对象及索引回收；仍按原有安全条件执行。 |
| `event_run` | `SimulationRunner::run()`或`SimulationSession::drive()`，包含事件执行、状态检查及同步回调。 |
| `wait_notify` | 发布通知、查找订阅者、安排唤醒事件的程序时间。 |
| `wait_callback` | 通知注册表实际执行回调的时间，包含回调内部工作。 |
| `read_wakeup` | ReadPath状态观察回调，通常嵌套在wait_callback中。 |
| `device_pump` | Device推进Ingress、缓存、介质候选及响应路径的时间。 |

每项包含`calls`、`unwound_calls`、`inclusive_ns`、`self_ns`和`max_ns`。`unwound_calls`表示异常展开经过该作用域，不等同于业务错误数。

- `inclusive_ns`包含子调用；`self_ns`只扣除**已插桩的子作用域**，未插桩的库调用／Device执行／观察器等仍计入父项。
- 因此`prepare.inclusive_ns`与`split.inclusive_ns`不能相加；`event_run.self_ns`也不能直接命名为“Device耗时”。
- `covered_root_ns`是没有计时父作用域的区间之和，避免嵌套重复计费；不等于整个进程总墙钟时间。不包含GPU计算、未包围的适配层代码、文件导出等区间。
- 墙钟会包含被操作系统抢占的时间。插桩不扣除自身开销，计数器在循环内也有成本；必须保留同一构建、相同输入的开关对照。

## 工作量计数

| 计数器 | 口径 |
|---|---|
| `split_fragments` | 成功构造的Fragment总数，含最终因容量背压未登记的输入，以及校验失败前已生成的片段。 |
| `prepare_globals` | 进入prepare的Global条目数，含重试提交。 |
| `registered_globals / registered_fragments` | 实际插入Host账本的Global／逻辑片段。内部命令重试不重新登记逻辑输入。 |
| `input_registered / input_backpressured / input_too_large` | 增量提交的批次数，分别记录三种返回结果；不含抛出的输入契约错误。 |
| `backpressured_fragments` | 本次已经prepare、随后因剩余容量不足被拒绝的片段数，可能重复计同一外部请求。 |
| `pump_queue_visits / pump_candidates` | 队列元素访问数／每次选择时合法候选数量之和，同一候选可能重复统计。 |
| `handoff_attempts / handoff_taken` | 进入`port.try_submit`的次数／返回Taken的次数，不是Global数量。 |
| `results_entries_visited` | 兼容旧报告保留；当前Host完成查询不扫描历史，值为0。旧版记录实际谓词调用次数。 |
| `results_size_peak` | 成功登记时观察到的历史结果表最大条目数；保留历史摘要的内存仍存在。 |
| `reclaim_unit_visits` | 回收扫描访问的Unit数，不表示这些Unit已被删除。 |
| `reclaimed_work / reclaimed_units / reclaimed_requests` | 实际删除的详细对象数，保留的results摘要与retention不算回收。 |
| `read_arms / read_wakeups / read_rearms` | 读状态订阅建立次数／回调执行次数／回调未形成终结结果的次数；不是读请求数。 |
| `read_wake_resolved` | 读状态回调实际形成了数据或错误结果的次数，不包含普通介质发射或Host交付。 |
| `read_result_blocked` | 已发现可终结的数据结果，但reserve失败的检查次数。重复检查重复计数。 |
| `waits_peak` | 注册表建立订阅时观察到的驻留订阅数量峰值，包括已排队尚未执行的订阅。 |

## 通知原因诊断

JSON的`wait_reasons`按通知原因输出固定大小的汇总，schema 1采用新增字段扩展，既有区域和计数器保留。

| 字段 | 口径 |
|---|---|
| `notifications` | 发布该原因的次数，包括没有订阅者的通知。 |
| `subscriber_hits` | notify时仍挂在该键上的订阅者数量之和。一次订阅排队后从所有键解除，因此重复通知不会再次命中它。 |
| `queued / callbacks / cancelled` | 实际安排的唤醒数／实际执行数／执行前取消数。暂停时queued可包含尚未执行的事件。 |
| `revision_wakes` | 订阅时发现观察版本已变化而安排的唤醒数；属于queued的子集，不另加。 |
| `callback_ns` | 对应回调的inclusive程序墙钟时间，包含read_wakeup等子作用域。 |

回调归因于**首先使该单次订阅进入队列的原因**；版本差异同时存在时取订阅列表中第一个变化原因。它不是所有可能原因的独立因果分摊。不能把subscriber_hits、queued和callbacks相加，或把callback_ns与read_wakeup时间相加。回调中再次订阅计为新订阅；功能上有价值的Bank唤醒不包含在read_wake_resolved中。

`physical_users`／`zone_users`只表达使用者退出的排空进展；`physical_state`表达物理可用性变化；`zone`表达Zone接收／映射状态。逻辑读逐次退出、物理lease逐次退出均登记通知；同一稳定发布边界前的物理释放可以合并，不在析构中执行回调。读状态观察不订阅使用者退出。`result_space`只供真实空间等待者使用，生产Egress按channel隔离预算和通知，默认共享的组件测试存储保持共享唤醒范围。对所有原因的回调计数应结合具体消费者判断，不能要求它们都为零。

诊断只读现有状态并累加固定大小的计数，不额外扫描容器。Host的历史完成扫描已经替换为精确计数，见[设计决策](design_decisions.md#adr-host-efficiency-01)。`admit_input()`的剩余容量检查仍在prepare之后；重复准备、pump和reclaim的成本仍需分别测量。JSON schema仍为1，新增`completion_check`，旧扫描字段保留为0，避免把常数查询冒充历史扫描。

## 不需要GPU的复现

并发读通知基准使用`hbf_eventlab_read_wakeup_bench CHANNELS PAGES_PER_CHANNEL READ_PASSES [PROFILE_JSON|-] [EVENTS_JSONL]`。通过原Host完整写入初始化，再根据Global完成反馈提交整段读，覆盖DataToken并检查最终排空。每channel为1die／1bank，8page/block，按页数补足容量；Selected/Supported MOCS为1024，读为Regular，窗口10,000,000 Tick、数量阈值关闭，Host驻留额度等于整段片段数。输入范围：channel 1..16、页数1..16、读取遍数1..128；这些是诊断输入，不是校准后的GPU工作负载。

```powershell
.\build\Release\hbf_eventlab_read_wakeup_bench.exe 4 4 3
.\build\Release\hbf_eventlab_read_wakeup_bench.exe 4 16 2 build\wakeups.json build\business-events.jsonl
```

增大页数会同时增大并发候选数；固定参数比较优化前后，使用同一初始化及输入。比较诊断开关时，事件总数和业务记录都应一致。比较通知优化前后，内部无效事件数允许下降，业务记录、结果和可观察完成Tick应保持一致。计时比较应关闭诊断及逐事件导出；与诊断/事件流对照分开运行。CTest只检查正确性和无关唤醒计数，不设置墙钟速度阈值。

`hbf_eventlab_runtime_bench PAGES READ_PASSES [PROFILE_JSON]`使用原tensor适配器、Host和Device：初始化指定页数，随后整段读取若干遍。默认配置为1channel／1die／2bank、32page/block、resident=64、Global chunk=4096、fragment64；几何按页数补足，仍用符号内容和未校准功能时序。无逐命令日志，无CUDA依赖。

```powershell
.\build\Release\hbf_eventlab_runtime_bench.exe 32 4
.\build\Release\hbf_eventlab_runtime_bench.exe 32 4 build\profile-32.json
.\build\Release\hbf_eventlab_runtime_bench.exe 128 4 build\profile-128.json
.\build\Release\hbf_eventlab_runtime_bench.exe 128 24
```

stdout包含程序墙钟、最终Tick、事件数、Global数、响应数和介质操作数。比较开关时除墙钟外这些字段应一致。建议交替开关、重复至少三次并报告中位数；不要在构建或其他重负载验证同时运行时比较墙钟。CTest不设性能阈值，避免机器速度影响功能验证。

可选Qwen可在原命令后加`--profile on`，得到`<output-prefix>.profile.json`；默认`off`，仅HBF模式可开启。实验程序自行管理输出前缀，不使用核心INI的输出路径。`--detail off --profile on`保留聚合诊断、关闭大量逐命令统计，仍保留原有访存／进度报告。其他选区、权重、计算间隔及数值核验要求见[Qwen实验](qwen_online.md)。

优先观察：split占比是否大；输入背压导致多少重复拆分；pump、reclaim是否也有大量重复扫描。对旧报告可观察`results_entries_visited / results_scan.calls`随历史增长的趋势；当前两项均为0，不应计算0/0。这些是优化线索，不应把单项较大直接解释为唯一瓶颈。

## 验证

`runtime_profile_*`覆盖确定性假时钟下的嵌套／自身计时、异常展开、隔离、输出错误、默认关闭、增量容量拒绝计数，以及普通完成、program-fail replay、Pending满重试和组装超时重试的开关对照。对照逐字比较模拟事件流，同时检查Tick、事件数、最终结果和排空。CLI测试检查配置、JSON字段、输出路径冲突；CPU基准检查历史表峰值、重复拆分和开关语义一致。

`host_completion_*`在每个统计观察点和事件返回后，用测试侧全表扫描核对计数查询。12个场景覆盖拒绝的原子性、未来释放、反馈新增、replay、Pending/timeout重试、依赖取消、恢复失败、详细对象回收和跨Global burst。传入`--snapshot FILE`可导出确定性事件流及结果摘要，用于跨版本逐字比较；生产代码不执行测试侧扫描。

<a id="completion-count-comparison"></a>

## 历史完成查询优化对照（2026-10-27）

基线为`ab821bc`，对照版本使用[Global完成计数](design_decisions.md#adr-host-efficiency-01)。同一Windows机器、MSVC 19.43 x64 Release，使用上述CPU基准；两版本各预热一次`32 4`，然后每个规模交替运行三次，期间不并行执行构建或测试。下表是**关闭诊断**时基准自身报告的程序墙钟中位数，括号为最小—最大值；不包含进程启动、GPU计算或模型文件加载。

| 初始化页数 × 读取遍数 | Global总数 | 基线（秒） | 优化后（秒） | 中位耗时减少 |
|---|---:|---:|---:|---:|
| 32 × 4 | 160 | 0.877（0.858—0.887） | 0.832（0.829—0.832） | 5.1% |
| 128 × 4 | 640 | 4.036（3.949—4.048） | 3.423（3.423—3.451） | 15.2% |
| 128 × 24 | 3200 | 33.042（32.626—33.973） | 20.755（20.520—20.786） | 37.2% |

全部对照运行的最终Tick、事件数、Global数、响应数、program/sense/cache-hit数量一致。最大案例均为3,561,761个事件、204,800次命令响应；另外12个生命周期场景使用相同测试驱动，优化前后事件流及最终结果快照逐字一致。完整Debug/Release回归见[验证记录](verification.md)。

单独开启诊断运行`128 24`：基线的完成扫描实际访问1,300,936,256个历史条目，812,953次扫描累计11.388秒；优化后旧扫描区域及访问次数均为0，812,953次`completion_check`累计0.034秒。后者包含插桩开销，不应解释为整数比较本身的耗时。两版结果表峰值仍为3200，历史摘要没有因优化而被丢弃。

这些结果只说明当前CPU基准中的程序效率；小案例收益较小。它们不是硬件性能、全权重Qwen运行时间或跨平台加速保证。历史摘要的内存、发送／回收／恢复扫描、输入背压前的重复准备仍需后续定位。

## 并发读通知优化对照（2026-11-01）

基线为`18db786`加上相同的通知诊断和`hbf_eventlab_read_wakeup_bench`，对照版本采用[按实际条件订阅](design_decisions.md#adr-runtime-efficiency-01)。两版在同一Windows机器使用MSVC 19.43 x64 Release；每组各预热一次，再交替运行三次，期间没有并行构建或测试。下表是**关闭诊断和逐事件导出**时的程序墙钟中位数，仅覆盖输入提交到会话运行结束，不含进程启动、会话构造和GPU计算。

| channel数 × 每channel页数 × 读取遍数 | 优化前（秒） | 优化后（秒） | 中位耗时减少 | 内部事件数（前 → 后） |
|---|---:|---:|---:|---:|
| 1 × 4 × 3 | 0.378 | 0.197 | 48.0% | 93,214 → 17,196 |
| 4 × 4 × 3 | 2.413 | 1.459 | 39.5% | 372,856 → 68,784 |
| 4 × 16 × 2 | 33.020 | 19.311 | 41.5% | 4,460,055 → 348,375 |

三组的最终Tick、响应数、program/sense/cache-hit数量及成功排空结果均保持一致。另行开启逐事件导出核对，三组业务事件文件分别与基线逐字节一致，包括时间与记录次序；被删除的是无效内部通知事件，不能要求事件引擎的总执行次数也不变。

最大案例单独开启诊断：ReadPath状态回调从4,111,680次降至0，其中基线3,881,518次由ResultSpace触发、230,162次由Zone触发，全部未产生终结结果。优化后正常读仍建立8,192次状态订阅；该基准没有需要ResultSpace等待的终结转发，也没有故障或映射变化，因此零回调符合输入条件。不能将其推广为“所有读回调均应删除”；空间不足、物理故障、Zone状态变化、释放回滚和通知版本竞争由`notification_*`测试单独验证。

该比较只衡量合成并发读下的软件效率，不是完整GPU工作负载或真实HBF性能结果。发送/回收扫描以及SenseOrder、BlockProgress引发的其他检查仍保留；默认诊断关闭，完整回归结果见[验证记录](verification.md)。
