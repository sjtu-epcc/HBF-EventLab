# 程序性能诊断

本模块测量**模拟器程序自身的墙钟耗时**，不测量模拟中的HBF延迟，不改变Tick、事件排序、64B拆分、Host容量、重试或回收条件。默认关闭，与`--stats-json`／`--stats-events`独立。

## 启用方式

普通CLI通过统一参数入口启用：

```powershell
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --runtime-profile build\runtime.json
# 等价：--set output.runtime_profile_json=build/runtime.json
```

INI对应`[output] runtime_profile_json="build/runtime.json"`；空字符串关闭。输出不得覆盖workload、输入配置或其他输出。正常、逻辑失败及运行限额停止都可导出；进程被强制终止不会保存内存中的最终汇总。输入错误发生在运行前时，不保证生成报告。

C++调用方创建`RuntimeProfile`，在开始输入前调用`HostDriver::set_runtime_profile(profile)`；在线路径可通过`SimulationSession(resolved, sink, &profile)`或`TensorAccessBridge(resolved, regions, bridge_config, sink, &profile)`传入。传`nullptr`关闭。对象由调用方持有，必须比Host／Session／Bridge活得更久；同一实例仅用于同一模拟线程。Session不自动解释输出路径或创建文件。

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
| `advance` | Host状态推进，包括恢复／重试及历史完成检查。 |
| `results_scan` | advance中实际的`std::all_of(results_)`，保留原有提前退出行为。 |
| `reclaim` | 详细Work／Unit／Global对象及索引回收；仍按原有安全条件执行。 |
| `event_run` | `SimulationRunner::run()`或`SimulationSession::drive()`，包含事件执行、状态检查及同步回调。 |

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
| `results_entries_visited` | advance的完成检查实际调用谓词次数，不用“表大小×调用数”估算。 |
| `results_size_peak` | 成功登记／执行完成检查时观察到的历史结果表最大条目数。 |
| `reclaim_unit_visits` | 回收扫描访问的Unit数，不表示这些Unit已被删除。 |
| `reclaimed_work / reclaimed_units / reclaimed_requests` | 实际删除的详细对象数，保留的results摘要与retention不算回收。 |

诊断只读现有状态并累加固定大小的计数，不额外扫描容器。特别是**没有**提前移动`admit_input()`的剩余容量检查，也没有优化`results_`扫描；这些成本正是待测对象。

## 不需要GPU的复现

`hbf_eventlab_runtime_bench PAGES READ_PASSES [PROFILE_JSON]`使用原tensor适配器、Host和Device：初始化指定页数，随后整段读取若干遍。默认配置为1channel／1die／2bank、32page/block、resident=64、Global chunk=4096、fragment64；几何按页数补足，仍用符号内容和未校准功能时序。无逐命令日志，无CUDA依赖。

```powershell
.\build\Release\hbf_eventlab_runtime_bench.exe 32 4
.\build\Release\hbf_eventlab_runtime_bench.exe 32 4 build\profile-32.json
.\build\Release\hbf_eventlab_runtime_bench.exe 128 4 build\profile-128.json
```

stdout包含程序墙钟、最终Tick、事件数、Global数、响应数和介质操作数。比较开关时除墙钟外这些字段应一致。建议交替开关、重复至少三次并报告中位数；不要在构建或其他重负载验证同时运行时比较墙钟。CTest不设性能阈值，避免机器速度影响功能验证。

可选Qwen可在原命令后加`--profile on`，得到`<output-prefix>.profile.json`；默认`off`，仅HBF模式可开启。实验程序自行管理输出前缀，不使用核心INI的输出路径。`--detail off --profile on`保留聚合诊断、关闭大量逐命令统计，仍保留原有访存／进度报告。其他选区、权重、计算间隔及数值核验要求见[Qwen实验](qwen_online.md)。

优先观察：split占比是否大；输入背压导致多少重复拆分；results扫描平均访问数`results_entries_visited / results_scan.calls`是否随历史增长；pump、reclaim是否也有大量重复扫描。它们是优化线索，不应把单项较大直接解释为已经证明的唯一瓶颈。

## 验证

`runtime_profile_*`覆盖确定性假时钟下的嵌套／自身计时、异常展开、隔离、输出错误、默认关闭、增量容量拒绝计数，以及普通完成、program-fail replay、Pending满重试和组装超时重试的开关对照。对照逐字比较模拟事件流，同时检查Tick、事件数、最终结果和排空。CLI测试检查配置、JSON字段、输出路径冲突；CPU基准检查历史表峰值、重复拆分和开关语义一致。
