# HBF EventLab 验证

本文集中记录功能覆盖、可复现命令和已有结果。验证对象是明确的行为子集；通过测试不代表完整OCP合规或经过校准的硬件性能。设计与测试的对应关系见[设计决策](design_decisions.md)，能力边界见[支持范围与限制](scope_and_limitations.md)。

<a id="evidence"></a>

## 1. 验证版本与环境

默认完整测试集共 **360 项**（350项核心测试＋10项实验及公共基础设施测试入口，本次使用Python 3.14）。实验测试默认开启，缺少Python 3.10+会导致配置失败；显式关闭后为350项核心测试，不属于完整回归；下列回归在 Windows、MSVC 19.43、x64 环境完成。测试覆盖当前支持的功能子集，目标环境的复现命令见第 3、4 节；本表不代表其他工具链已完成本次回归。

| 配置 | 验证日期 | 测试结果 | CTest墙钟耗时 |
|---|---|---|---:|
| Debug | 2026-10-24 | 360/360通过，0失败 | 85.57s |
| Release | 2026-10-24 | 360/360通过，0失败 | 15.29s |

上述两个配置各按 `ctest --parallel 4` 运行，测试期间同时执行了其他验证工作；耗时只记录本次环境。墙钟耗时包含测试进程和测试框架开销，并受机器、工具链、并发和系统负载影响；不构成 Debug/Release 或跨平台性能结论。项目要求 C++17 和 CMake 3.20 或更新版本，平台构建方法见[使用说明](usage.md#构建)；复现时应记录 commit、工具链及配置。

## 2. 测试矩阵

360项由35个基础行为场景、34个生命周期/事件场景、44个Device场景、1个Device示例、103个Host行为场景、1个增量Host示例、32个在线会话场景、1个会话示例、35个统计场景、26个配置场景、28个CLI场景、10个读调度/窗口观察场景和10个实验及公共基础设施测试入口组成。分类表示验证职责，不表示规范章节数量。

| 验证面 | 代表场景 | 主要断言 |
|---|---|---|
| 地址与接收 | address_mapping、admission、shared_credit | 地址单位、双映射、共享credit与响应生命周期。 |
| 写组装与恢复语义 | overlap_restart、timeout_retry、replay_prefix、replay_merge | 覆盖预检查、同页重试、失败前缀与终点合并。 |
| 容量事务 | capacity_atomicity、capacity_hole_failure、retirement_write_swap | 失败不部分提交，实际物理所有权与永久逻辑退役。 |
| 事件与截止 | event_rounds、deadline_batch、deadline_credit_chain、deadline_two_dlus | 同Tick因果闭包、共同deadline边界、有效补片与RX驻留区别。 |
| 读保护与候选 | read_accept_issue、read_quiesce_drain、program_stale_candidate | 接受/发射分离，旧内容保护，陈旧ticket不能操作新上下文。 |
| 定向背压 | wait_targeting、wait_race_cancel、ingress_bypass | 不漏唤醒、可取消、合法请求可越过受阻者。 |
| Bank与sense顺序 | bank_parallel、bank_serial、sense_order_policy | 独立Bank并行，替换策略不能破坏同Bank顺序。 |
| Cache与结果存储 | cache_hit、miss_becomes_hit、tx_cache_pressure、rewrite_saved_read | 双槽保护、有限TX反压、旧结果不随重写改变。 |
| 读窗口与组序 | window_batch_timer、batch_pending、batch_missing、ordered_late_error | Batch边界、主动timer、Pending提示与组内错误返回。 |
| 链路与正式接受 | rx_reservation、mocs_ingress、link_latency、multi_channel_bandwidth | 在途RX预留、MOCS边界、序列化/传播分离。 |
| Global与发送策略 | splitter、sixteen_channels、host_send_fcfs、host_send_fill_open、host_send_blocked_open、host_send_channels | 跨Global补DLU；真实Device下两种策略次序不同，受阻请求可被合法补片越过，各channel独立前进。 |
| Host入队身份与次序 | host_send_release_order、host_send_same_tick、host_send_dependency_age、host_send_not_taken、host_send_burst_mixed、host_send_cross_global_burst | 按释放与输入次序入队，依赖／背压保留队龄；burst等最后贡献片段，不受预加载构造顺序影响。 |
| Host重试排序 | host_send_retry_tail、host_send_same_tick_retry、host_send_timeout_tail、host_send_timeout_unsent、host_send_replay_followers | 错误重发排到队尾，未发送补片保留原次序，同Tick可复现；稳定逻辑身份和统计释放时刻不被重置。 |
| Host恢复 | recovery_once、follower_before_root、replay_merge、burst_recovery | 真正响应驱动、根因定位、任务合并和恢复终点。 |
| Host命令重试 | pending_root_first、pending_follower_first、pending_late_followers、pending_burst | 根因或后继错误先到均可修复；只重发被拒绝命令，保留有效片段；burst仍按一条命令记账。 |
| Host容量反馈 | pending_progress、real_pending | 容量进展早于迟到错误时不漏唤醒，无用timer取消；实际Pending名额释放后可继续写入。 |
| Host超时重试 | timeout_late_fragments、timeout_release、real_timeout_success | 旧尝试全部终结后整页重发；等待未来补片的释放时刻，不把首次发送计为重传。 |
| 关联写序错误 | unexplained_order | 缺少可关联前页根因的0x6/info1明确失败，不无条件循环重试。 |
| 重试预算与关闭 | pending_exhausted、pending_disabled、real_timeout_budget、partial_timeout | 命令和整页预算分别有界，支持关闭重试；MOCS不足以收齐页时仍能失败并排空。 |
| 重试数据完整性 | timeout_missing_retention、timeout_missing_coverage | 保留区缺数据或输入未覆盖整页时明确失败，不从Device补出数据。 |
| 资源重试与replay衔接 | pending_during_replay、timeout_during_replay | replay内的资源修复不另建恢复任务，也不额外增加block replay遍数。 |
| 每遍排空 | delayed_old_attempts、ingress_old_attempts、late_read_second_pass | 迟到Link/RX/响应取得最终归属后才允许新replay。 |
| 数据与预算 | retention_pins、missing_retention、persistent_failure、prior_success_retained | 恢复数据身份、有限失败、历史成功与独立工作。 |
| 输入与停止 | input_validation、ordered_pending、runner_device_activity、cancelled_future_release | 能力诊断、双方排空、取消无用未来事件。 |
| 增量输入与释放 | host_incremental_split、host_incremental_future、host_incremental_future_cancel_reuse、host_incremental_burst、host_incremental_hints | 跨提交补齐DLU、同Tick释放合并、取消后重新登记、同批完整burst、每Global/channel末尾Batch。 |
| 增量校验与内容身份 | host_incremental_atomic、host_incremental_address_atomic、host_incremental_duplicate、host_incremental_sealed、host_incremental_block_scope、host_incremental_dependencies、host_incremental_invalid_dependencies、host_incremental_limit_atomic | 整批错误不留副作用、不封闭旧版本；ID与片段去重、非单调内容ID、按逻辑块封闭、依赖及累计额度。 |
| 增量反馈与恢复 | host_incremental_feedback、host_incremental_recovered_sealed、host_incremental_recovery_append、host_incremental_abort_append、host_incremental_demo | 完成通知追加工作；已封闭旧版本仍可replay；恢复中追加、失败后拒绝补片和恰好一次最终结果。 |
| 输入模式与等价性 | host_incremental_idle、host_incremental_equivalent、host_incremental_observer、host_incremental_parser、host_incremental_guards | 空闲后继续、离线/增量完整统计一致、通知不改变结果时刻、逐条解析与模式保护；有限Runner拒绝误判开放输入。 |
| 在线时间边界 | session_exclusive、session_empty_frontier、session_endpoint_input、session_horizon_guard、session_clock_idle、session_maximum_boundary | 排他终点未执行、空区间不可回填、不能跳过活事件、零时刻和最大时刻。 |
| 在线登记与来源 | session_first_atomic、session_late_atomic、session_open_drained、session_close_work、session_close_empty、session_missing_predecessor、session_closed_stalled | 首批拒绝不启动Host，批内晚到原子拒绝，来源开放/关闭与活动排空分开，后续输入可补前页。 |
| 在线反馈 | session_feedback_closed、session_feedback_guard、session_feedback_future、session_feedback_validation、session_causal_time、session_demo | 关闭后仍允许完成事件产生工作；禁止外部伪装反馈和重入，未来反馈阻止提前完成，不制造额外Tick。 |
| 在线限额与诊断 | session_limit_total、session_limit_tick、session_limit_round、session_time_empty、session_time_future、session_entered_tick、session_callback_error、session_callback_model_error | 跨暂停保留预算、显式提高后继续、空区间也受时间上限约束、异常不重复执行回调。 |
| 在线deadline与恢复 | session_open_timeout、session_frozen_deadline、session_deadline_endpoint、session_deadline_feedback、session_recovery、session_equivalent | 冻结批次不被反馈穿插，Host释放不能代替Device接受，旧timeout不能被复活；同一Host恢复跨暂停继续，连续与分段运行统计/事件数一致。 |
| 运行限额 | time_limit、event_limit、same_tick_limit、round_limit | 分别触限、现场保留，不制造完成或额外Tick。 |
| 统计时间与归属 | statistics_boundaries、statistics_ingress_wait、statistics_cross_global_burst、statistics_multi_channel | 五个时刻、credit持有时长、跨Global burst一次响应、跨channel最后逻辑结果决定Global完成。 |
| 统计重试与终止 | statistics_pending_retry、statistics_timeout_retry、statistics_replay、statistics_partial_future_cancel、statistics_unfinished_resume | 重试不改逻辑分母/释放时刻；部分子工作提前取消、限额停止及恢复继续不丢失或伪造样本。 |
| 统计观察与导出 | statistics_event_time、statistics_observational、statistics_zero_and_status、statistics_cli_normal、statistics_cli_limit | 使用事件时间；开关统计不改Tick/事件数；同数值读写状态分开；JSON结构、缺失时刻和输入保护。 |
| Pending生命周期与归属 | pending_statistics_lifecycle、pending_statistics_rejected、pending_statistics_timeout_reuse、pending_statistics_failure、pending_statistics_identity | 真实片段数及四个时刻、拒绝不造上下文、generation分开重试、program失败与后继失败、命令ID复用后的attempt归属。 |
| Pending曲线与阶段占用 | pending_statistics_occupancy、pending_statistics_curve_reconstruction、pending_statistics_same_tick_burst、pending_statistics_read_only | 各channel总量/阶段分区、峰值、满额时长、积分与均值；按变化点重建积分，同Tick零时长和纯读零曲线。 |
| Pending提交与停止边界 | pending_statistics_commit_failure、pending_statistics_device_delivery、pending_statistics_snapshot、pending_statistics_device_limit、pending_statistics_teardown、statistics_cli_assembly_limit | program提交失败不退上下文；Pending释放早于响应/MOCS；中断保留活动尾段，继续运行不重复计数，析构不伪造终结。 |
| Bank统计 | statistics_bank_lifetime、statistics_bank_limit、statistics_bank_guards | sense／program／erase的实际发射与释放；在途快照、继续执行、独立Bank并行及非法／重复观察。 |
| 读调度与窗口观察 | read_scheduling_policy_order、read_scheduling_window_reasons、read_scheduling_window_stale、read_scheduling_cache_bypass、read_scheduling_cache_removal | 非抢占读优先排序；数量／timer／Batch原因；失效timer；Cache绕过或移出窗口。 |
| 读观察边界 | read_scheduling_terminal_read、read_scheduling_snapshot、read_scheduling_observational、read_scheduling_guards、read_scheduling_custom_reason | 立即错误不造介质操作；暂停继续；开关统计不改事件/时间/结果；非法观察诊断；自定义策略兼容。 |
| 读窗口与NAND调度实验 | experiment_read_scheduling_unit、experiment_read_scheduling_integration | 7项Python单元检查；14例真实CLI重复运行一致，比较候选竞争／未释放／不抢占、末尾单读和Cache转换；输入公平性、覆盖保护和损坏统计拒绝。 |
| 实验公共层与测试依赖 | experiment_common_unit、experiment_python_configuration | 8项Python检查：实际配置／请求一致性、重复键、几何与预算派生、区间语义、输出及公共源码哈希；CMake验证Python可用、缺失及显式关闭三种路径。 |
| 合成容量实验 | experiment_capacity_unit、experiment_capacity_integration | 9项Python单元检查；真实CLI下credit分散边界、Pending拒绝、运行限额、重复运行一致性、坏记录与覆盖保护。 |
| 交织与到达间隔实验 | experiment_interleave_arrival_unit、experiment_interleave_arrival_integration | 11项Python单元检查；完整local页覆盖、共享trace、解析释放跨度、真实超时与取消、deadline精确边界、单channel退化、多Bank利用率、限额停止及损坏统计拒绝。 |
| 到达形态与Host策略 | experiment_arrival_policy_unit、experiment_arrival_policy_integration | 6项Python单元检查；相同trace与资源预算、真实策略选择、稀疏无竞争对照、局部与整体完成时间区分、重复确定性、覆盖保护及损坏记录拒绝。 |
| 配置格式与装配 | configuration_parse_override、configuration_numeric、configuration_frozen、configuration_cli_precedence | 字段类型、未知／重复输入、覆盖优先级和解析后冻结。 |
| Host策略配置 | configuration_cli_host_default、configuration_cli_host_fill_open、configuration_cli_host_override | 默认FCFS、文件选择fill_open、CLI覆盖及有效配置导出一致。 |
| 配置合法性与运行 | configuration_grade_limits、configuration_mocs_ranges、configuration_small_read、configuration_small_write、configuration_small_burst、configuration_capacity | 档位上限、Supported／Selected、实际写模式诊断、真实超时／burst前进及预算传递。 |
| 配置复现与保护 | configuration_roundtrip、configuration_default_equivalence、configuration_cli_roundtrip、configuration_cli_collision | 导出再载入保持结果／Tick／事件数；输入与输出不互相覆盖。 |

场景名指测试函数或CLI案例名；同名案例按“生命周期 / Device / Host / CLI”区分。Host断言主要位于 [host.cpp](../tests/host.cpp)、[host_retry.cpp](../tests/host_retry.cpp)、[host_send_policy.cpp](../tests/host_send_policy.cpp) 和 [host_incremental.cpp](../tests/host_incremental.cpp)；在线会话断言位于 [session.cpp](../tests/session.cpp)。实际CTest注册以[构建定义](../CMakeLists.txt)和`ctest -N`为准。设计表中的测试引用指向对应注册位置；需要查看断言时可在`tests/`中搜索函数，例如：

```text
rg -n "void event_rounds" tests
```

端到端Host测试提交Global workload和独立故障输入，由Device自动完成和交付；可控公共端口替身用于固定响应乱序和迟到片段等交错，`real_pending`、`real_timeout_success`和`real_timeout_budget`直接接入DeviceDriver与SimulationRunner。测试可以观察内部活动做断言，但生产Host不读取Device私有恢复状态。保守replay按页等待，不为构造交错而在数据未齐时提前制造program失败。

命令尝试与逻辑工作分别检查：错误响应数、实际重发命令数、整页重试轮数和最终逻辑成功数不能互相替代。关联规则与适用边界统一见[设计决策](design_decisions.md)。

## 3. 复现构建与测试

在新的源码checkout根目录使用独立构建目录（不要复用另一个生成器的缓存）：

```powershell
git rev-parse HEAD
cmake --version
cmake -S . -B build-verify -G "Visual Studio 17 2022" -A x64 -DHBF_EVENTLAB_EXPERIMENT_TESTS=ON
cmake --build build-verify --config Debug
ctest --test-dir build-verify -C Debug --output-on-failure --parallel 4
cmake --build build-verify --config Release
ctest --test-dir build-verify -C Release --output-on-failure --parallel 4
```

若采用其他编译器，应单独记录其验证结果。功能测试耗时是模拟器程序墙钟时间，不是HBF时延。可以列出全部测试，或按名称选择一个场景、一组功能：

```powershell
ctest --test-dir build-verify -C Release -N
ctest --test-dir build-verify -C Release -R '(^|_)recovery_once$' --output-on-failure
ctest --test-dir build-verify -C Release -R '^host_retry_' --output-on-failure
ctest --test-dir build-verify -C Release -R '^host_send_' --output-on-failure
ctest --test-dir build-verify -C Release -R '^host_incremental_' --output-on-failure
ctest --test-dir build-verify -C Release -R '^session_' --output-on-failure
ctest --test-dir build-verify -C Release -R '^statistics_' --output-on-failure
ctest --test-dir build-verify -C Release -R '^pending_statistics_' --output-on-failure
ctest --test-dir build-verify -C Release -R '^configuration_' --output-on-failure
```

Linux 的构建与全量回归命令见 [贡献指南](../CONTRIBUTING.md#testing-and-verification)，依赖安装见 [README 快速开始](../README.md#quick-start)。列出 Linux 测试使用 `ctest --test-dir build-linux-release -N`；详细日志由 CTest 保存在各构建目录的 `Testing/Temporary/LastTest.log`。

## 4. CLI结果与复现

以下使用上一节构建的Release可执行文件。每条命令执行后用`$LASTEXITCODE`核对退出码；失败示例中的2或4是预期结果，不能统一当成执行故障。

```powershell
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --burst
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --open 4 --fail-sequence 2
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --open 4 --fail-sequence 2,1
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --persistent-page 0
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --fail-sequence 0 --retention 0
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\partial.txt
.\build-verify\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt --max-tick 100
```

| 案例 | 预期exit | 结果摘要与检查项 |
|---|---:|---|
| normal | 0 | 256次移交=接受=响应，256个逻辑片段成功，2次program、2次sense、126次Cache命中。 |
| normal + burst | 0 | 移交命令130条：2个完整写事务及128个64B读。 |
| page 2失败一次 | 0 | 128个尝试错误，1任务/1遍，终点page 2；1280个逻辑片段最终成功。 |
| page 2失败后page 1再失败 | 0 | 192个尝试错误，仍1任务/2遍，终点保持page 2。 |
| page 0持续失败 | 2 | 三遍后RecoveryBudgetExhausted；独立Global 3/4成功，双方排空。 |
| retention=0 | 2 | MissingRecoveryData，0遍；独立工作成功，双方排空。 |
| partial | 2 | 已接受写以AssemblyTimeout（写状态5）终结；因输入未覆盖完整local页，Host报告`WriteStatus:5 MissingRetryData`，不伪造缺片。 |
| max-tick=100 | 4 | LimitReached，未完成工作保留。 |

正常案例的输出节选：

```text
STOP Completed workload=Success tick=1753065 events=19846 reason=All logical work succeeded
HOST handed=256 accepted=256 responses=256 attempt_errors=0 logical_success=256 logical_failed=0 recovery_tasks=0 replay_passes=0 replay_commands=0 pending_limit_errors=0 pending_retry_commands=0 assembly_timeout_errors=0 timeout_retry_rounds=0 timeout_retry_commands=0 order_retry_commands=0
DEVICE programs=2 erases=2 senses=2 cache_hits=126 host_drained=1 device_drained=1 pending_global=0 source_queue=0 unsettled_host=0 outstanding_device=0 ingress=0 pending_dlu=0 bank_ops=0 egress=0
```

这些精确Tick和计数属于已列配置及确定性实现的回归观测，不是性能校准目标。参数或策略变化后不能要求所有计数相同，应重新解释对应不变量。

上表八类CLI案例均通过退出码和输出检查。写重试的额外计数含义见[使用说明](usage.md#写重试统计)；可添加 `--pending-retries 0 --timeout-retries 0` 对比关闭资源重试的基线，此配置不关闭program-fail replay。直接运行示例的命令见 [README](../README.md#quick-start)。

另外三项CLI统计用例启用`--stats-json`，分别解析完整运行、传输中停止及组装中停止的输出；断言Global/逻辑/尝试数量、时间顺序、缺失时间为null、Pending曲线与上下文一致，并检查输出路径不能覆盖输入。API的17项请求统计场景、15项Pending统计场景和三项CLI案例合计35项。接口、指标定义和导出示例见[统一统计](usage.md#统一统计)。

17项CLI配置案例另行验证文件覆盖、非法配置诊断、导出重载、路径保护、Host策略选择，以及MOCS=1下纯读、64B写和完整burst三种不同结果。配置与参数语义见[参数说明](parameters.md)。

## 5. 验证边界

- 针对性断言不能证明全部状态组合正确；有序Pending、完整AXI/ECC和广域异步故障不在支持范围内。
- 编译与CTest通过不验证默认时延、缓冲深度、成本或真实硬件吞吐。
- 非法输入、预期逻辑失败、Stalled和运行限额分别检查；仅进程返回或事件队列为空不足以判断逻辑成功。
- 新增行为或修复问题应补对应断言，并同步唯一正式[设计决策](design_decisions.md)中的依据与覆盖等级。

<a id="test-index"></a>

## 6. 追溯键与测试定位

`T01`至`T35`是基础决策的稳定测试追溯键，保留其含义；它们不是执行顺序。下表链接到当前测试函数，执行登记见[CTest定义](../CMakeLists.txt)。

| 键 | 场景 / 当前源码位置 |
|---|---|
| <a id="t01"></a>T01 | [address_mapping()](../tests/foundations.cpp#L27) |
| <a id="t02"></a>T02 | [admission()](../tests/foundations.cpp#L74) |
| <a id="t03"></a>T03 | [write_lifecycle()](../tests/foundations.cpp#L103) |
| <a id="t04"></a>T04 | [events_dispatch()](../tests/foundations.cpp#L157) |
| <a id="t05"></a>T05 | [capacity_compensation()](../tests/capacity_retirement.cpp#L67) |
| <a id="t06"></a>T06 | [capacity_density()](../tests/capacity_retirement.cpp#L93) |
| <a id="t07"></a>T07 | [capacity_atomicity()](../tests/capacity_retirement.cpp#L113) |
| <a id="t08"></a>T08 | [capacity_hole_failure()](../tests/capacity_retirement.cpp#L158) |
| <a id="t09"></a>T09 | [capacity_lease_boundary()](../tests/capacity_retirement.cpp#L177) |
| <a id="t10"></a>T10 | [capacity_chain()](../tests/capacity_retirement.cpp#L200) |
| <a id="t11"></a>T11 | [replacement_pipeline()](../tests/write_path.cpp#L56) |
| <a id="t12"></a>T12 | [overlap_restart()](../tests/write_path.cpp#L100) |
| <a id="t13"></a>T13 | [timeout_retry()](../tests/write_path.cpp#L135) |
| <a id="t14"></a>T14 | [replay_prefix()](../tests/write_path.cpp#L174) |
| <a id="t15"></a>T15 | [replacement_failure()](../tests/write_path.cpp#L216) |
| <a id="t16"></a>T16 | [admission_boundaries()](../tests/write_path.cpp#L350) |
| <a id="t17"></a>T17 | [replay_merge()](../tests/write_path.cpp#L233) |
| <a id="t18"></a>T18 | [replay_replacement_merge()](../tests/write_path.cpp#L275) |
| <a id="t19"></a>T19 | [replay_remap_generation()](../tests/write_path.cpp#L299) |
| <a id="t20"></a>T20 | [retirement_write_swap()](../tests/write_path.cpp#L330) |
| <a id="t21"></a>T21 | [basic_errors()](../tests/read_path.cpp#L83) |
| <a id="t22"></a>T22 | [shared_credit()](../tests/read_path.cpp#L105) |
| <a id="t23"></a>T23 | [pending_forward()](../tests/read_path.cpp#L134) |
| <a id="t24"></a>T24 | [media_lifetime()](../tests/read_path.cpp#L153) |
| <a id="t25"></a>T25 | [recovery_reads()](../tests/read_path.cpp#L175) |
| <a id="t26"></a>T26 | [merged_read_gate()](../tests/read_path.cpp#L193) |
| <a id="t27"></a>T27 | [replacement_timeout_gate()](../tests/read_path.cpp#L207) |
| <a id="t28"></a>T28 | [retirement_reads()](../tests/read_path.cpp#L220) |
| <a id="t29"></a>T29 | [remap_read_generation()](../tests/read_path.cpp#L231) |
| <a id="t30"></a>T30 | [event_error_order()](../tests/read_path.cpp#L245) |
| <a id="t31"></a>T31 | [deadline_forward()](../tests/read_path.cpp#L267) |
| <a id="t32"></a>T32 | [quiesce_reads()](../tests/read_path.cpp#L286) |
| <a id="t33"></a>T33 | [read_issue_snapshot()](../tests/read_path.cpp#L300) |
| <a id="t34"></a>T34 | [ingress_scope()](../tests/read_path.cpp#L312) |
| <a id="t35"></a>T35 | [read_teardown()](../tests/read_path.cpp#L325) |

## 7. 合成容量实验复现

[容量实验说明](../experiments/capacity/README.md)提供8组smoke、32组pilot和648组完整计划。8组smoke及32组pilot已完成扫描和统计核对，均无工具错误；其中逻辑失败是实验观察结果，不等于测试失败。完整648组计划不作为此次已完成实验报告。

固定8个原始DLU、Pending上限8、program 0.5μs时：逐页发送均成功；轮转发送在Host打开上限4／MOCS 256时成功8/8，在上限5／MOCS 256时成功3/8、产生10个超时上下文；MOCS提高到512后成功8/8。失败格实际打开13个上下文，因此超时比例为10/13，不能写成5/8。链路／RX中旧片段的迟到处理可能再打开上下文，关闭自动重试并不保证上下文数等于原始页数。

pilot还覆盖Pending上限2与program 8μs；这些结果只验证当前有限输入和功能参数的行为，不能据此宣称真实HBF吞吐或通用最优策略。完整输入、有效参数、原始JSON、逐格退出状态和CSV由扫描脚本自动保存。可选绘图与CTest分离；图形依赖和命令见实验说明。

```powershell
python experiments/capacity/run.py --cli build/Release/hbf_eventlab_cli.exe --plan experiments/capacity/plans/pilot.json --output out/capacity-pilot
ctest --test-dir build -C Release -R "statistics_bank_|experiment_capacity_" --output-on-failure
```

## 8. 交织与到达间隔实验复现

[交织与到达间隔实验说明](../experiments/interleave_arrival/README.md)提供4组smoke及64组完整小矩阵，固定4 channel、FCFS、64B写、每channel MOCS 1024/Pending 16，扫描两种输入大小、四种交织粒度和八种释放间隔。64组均已完成并通过地址覆盖、时间与资源积分核对；44组所有原始DLU成功，20组出现预期逻辑失败，没有工具错误。每channel的实际MOCS/Pending峰值分别不超过512/8。

Δ=50ns时，16KiB输入在64B交织下4个上下文超时、0/4原始DLU成功；4096B交织下4/4成功。128KiB输入在64B交织下仍只有4个上下文超时，但Host取消同块后续工作，最终0/32成功；4096B交织下32/32成功。不能将取消的后续页计成独立超时，也不能将提前失败排空视作更短的成功完成时间。

计划释放跨度是独立对照量，实际组装和Bank操作来自统一统计观察。结果是当前功能参数下的有限输入实验，图表和复现方法见实验说明。

```powershell
python experiments/interleave_arrival/run.py --cli build/Release/hbf_eventlab_cli.exe --output out/interleave-arrival-round1
ctest --test-dir build -C Release -R '^experiment_interleave_arrival_' --output-on-failure
```

## 9. 到达形态与Host策略最小对照

[最小例子说明](../experiments/arrival_policy/README.md)固定一个channel/Bank、两个独立block的首页，提供成批和稀疏两种输入，每种输入分别运行`fcfs`与`fill_open`。四次运行均成功，输入哈希一致性和有效配置差异检查通过；没有修改模拟器的组装、调度或统计行为。

成批输入在2μs同时提供B的前半页和A的后半页，文件中B在前。FCFS观察到`A32 B32 A32 B32`的移交次序，`fill_open`为`A64 B64`。后者使A的program完成时间从3.501767μs提前到2.553444μs；由于B的后半页仍需等到4μs，两者整体逻辑完成时间同为4.574920μs。稀疏输入的两种策略结果一致。更早补齐一页不等于缩短整份workload的完成时间。

```powershell
python experiments/arrival_policy/run.py --cli build/Release/hbf_eventlab_cli.exe --output out/arrival-policy-minimal
ctest --test-dir build -C Release -R '^experiment_arrival_policy_' --output-on-failure
```


## 读窗口与 NAND 调度受控实验

[实验说明](../experiments/read_scheduling/README.md)提供14个有限案例，均完成并通过观测一致性检查。测量前正常写入读目标，Host固定FCFS，64B命令；两种NAND策略与不同窗口配置共享输入。

在窗口50ns的同Bank竞争中，FCFS先program后sense，读端到端延迟1.403534μs；读优先先sense后program，读延迟0.903534μs，同时W的program完成延后0.1μs。窗口1000ns时，读尚未释放，两个策略均先program。晚到读对照同样不抢占已发射program。

同一份四冷读输入下，阈值4在10.120434μs释放，固定200ns的timer在10.200434μs释放；尾部三个读及单读不依赖后续输入，也能定时释放。同页Cache对照仅一次sense，两次Cache服务，其中一条读在新窗口释放前被Cache服务并移出。数据初始化不计入上述测量；这些是未校准功能模型结果。

```powershell
python experiments/read_scheduling/run.py --cli build/Release/hbf_eventlab_cli.exe --output out/read-scheduling-minimal
python experiments/read_scheduling/plot.py out/read-scheduling-minimal
```

生成/运行/分析使用Python标准库；绘图需要matplotlib。输出目录必须是新目录。此实验报告逐请求延迟和最大值，不把少量样本的分位数作为真实workload的尾延迟结论。

公共层重构的结果保持性检查：对已有32个容量案例、4个交织案例、4个Host策略案例、14个读调度案例，共54个案例的原始统计和有效配置进行独立副本重分析，全部已有汇总指标一致（不比较CLI进程墙钟耗时）。四组绘图入口均成功导出；共享层未改变C++执行语义。重分析副本保留原始来源并单独记录新源码哈希，不覆盖历史结果。
