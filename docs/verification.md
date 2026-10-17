# HBF EventLab 验证

本文集中记录功能覆盖、可复现命令和已有结果。验证对象是明确的行为子集；通过测试不代表完整OCP合规或经过校准的硬件性能。设计与测试的对应关系见[设计决策](design_decisions.md)，能力边界见[支持范围与限制](scope_and_limitations.md)。

<a id="evidence"></a>

## 1. 验证版本与环境

当前测试集共 **246 项**；下列回归在 Windows、MSVC 19.43、x64 环境完成。测试覆盖当前支持的功能子集，目标环境的复现命令见第 3、4 节；本表不代表其他工具链已完成本次回归。

| 配置 | 验证日期 | 测试结果 | CTest墙钟耗时 |
|---|---|---|---:|
| Debug | 2026-10-14 | 246/246通过，0失败 | 49.43s |
| Release | 2026-10-16 | 246/246通过，0失败 | 8.47s |

上述 CTest 使用 4 个并行作业。墙钟耗时包含测试进程和测试框架开销，并受机器、工具链、并发和系统负载影响；不构成 Debug/Release 或跨平台性能结论。项目要求 C++17 和 CMake 3.20 或更新版本，平台构建方法见[使用说明](usage.md#构建)；复现时应记录 commit、工具链及配置。

## 2. 测试矩阵

246项由35个基础行为场景、34个生命周期/事件场景、44个Device场景、1个Device示例、65个Host行为场景、17个统计场景、26个配置场景和24个CLI场景组成。分类表示验证职责，不表示规范章节数量。

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
| Global与发送策略 | splitter、sixteen_channels、burst_cross_global、fill_open | 跨Global补DLU，burst归属与合法补片优先。 |
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
| 运行限额 | time_limit、event_limit、same_tick_limit、round_limit | 分别触限、现场保留，不制造完成或额外Tick。 |
| 统计时间与归属 | statistics_boundaries、statistics_ingress_wait、statistics_cross_global_burst、statistics_multi_channel | 五个时刻、credit持有时长、跨Global burst一次响应、跨channel最后逻辑结果决定Global完成。 |
| 统计重试与终止 | statistics_pending_retry、statistics_timeout_retry、statistics_replay、statistics_partial_future_cancel、statistics_unfinished_resume | 重试不改逻辑分母/释放时刻；部分子工作提前取消、限额停止及恢复继续不丢失或伪造样本。 |
| 统计观察与导出 | statistics_event_time、statistics_observational、statistics_zero_and_status、statistics_cli_normal、statistics_cli_limit | 使用事件时间；开关统计不改Tick/事件数；同数值读写状态分开；JSON结构、缺失时刻和输入保护。 |
| 配置格式与装配 | configuration_parse_override、configuration_numeric、configuration_frozen、configuration_cli_precedence | 字段类型、未知／重复输入、覆盖优先级和解析后冻结。 |
| 配置合法性与运行 | configuration_grade_limits、configuration_mocs_ranges、configuration_small_read、configuration_small_write、configuration_small_burst、configuration_capacity | 档位上限、Supported／Selected、实际写模式诊断、真实超时／burst前进及预算传递。 |
| 配置复现与保护 | configuration_roundtrip、configuration_default_equivalence、configuration_cli_roundtrip、configuration_cli_collision | 导出再载入保持结果／Tick／事件数；输入与输出不互相覆盖。 |

场景名指测试函数或CLI案例名；同名案例按“生命周期 / Device / Host / CLI”区分。Host断言主要位于 [host.cpp](../tests/host.cpp) 和 [host_retry.cpp](../tests/host_retry.cpp)。实际CTest注册以[构建定义](../CMakeLists.txt)和`ctest -N`为准。设计表中的测试引用指向对应注册位置；需要查看断言时可在`tests/`中搜索函数，例如：

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
cmake -S . -B build-verify -G "Visual Studio 17 2022" -A x64
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
ctest --test-dir build-verify -C Release -R '^statistics_' --output-on-failure
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

另外两项CLI统计用例启用`--stats-json`，分别解析完整运行及限额停止的输出；断言Global/逻辑/尝试数量、时间顺序、缺失时间为null，并检查输出路径不能覆盖输入。API的17项统计场景与两项CLI案例合计19项。接口、指标定义和导出示例见[统一统计](usage.md#统一统计)。

14项CLI配置案例另行验证文件覆盖、非法配置诊断、导出重载、路径保护，以及MOCS=1下纯读、64B写和完整burst三种不同结果。配置与参数语义见[参数说明](parameters.md)。

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
