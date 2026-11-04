# HBF EventLab 统计事件流

`StatisticsEventStream`是`IStatisticsSink`的一种实现，逐条观察现有Host/Device事件，按序列化字节阈值写出JSON Lines。它不重复维护组装逻辑、不新增模拟事件，也不把flush当作请求完成。完整快照分析仍可使用`StatisticsCollector`和`--stats-json`；两者格式不同。

## 使用与生命周期

```powershell
./build/Debug/hbf_eventlab_cli.exe examples/workloads/normal.txt --stats-events events.jsonl --set output.statistics_buffer_bytes=65536
```

CLI正常取得运行报告后写入`StreamEnd`并flush，包括运行限额或逻辑失败；检查进程退出码和最后一条记录，不能把存在输出文件当作成功。路径不能覆盖输入、有效配置或完整统计JSON文件。输出文件按新运行创建，不支持重启进程后续写同一会话。

C++在线使用示例：

```cpp
std::ofstream output("events.jsonl", std::ios::binary);
StatisticsEventStream statistics(output, 65536);
SimulationSession session(resolve_configuration(config), &statistics);
// 登记输入并推进；暂停后可执行 statistics.flush()。
// 需要继续同一会话时只flush，不调用finish。
session.close_input();
auto report = session.run_to_completion();
statistics.finish(session.now(), to_string(report.run.status));
```

头文件为[statistics/event_stream.hpp](../include/hbf_eventlab/statistics/event_stream.hpp)。ostream及sink必须比生产者存活更久。API不自动解释output配置中的文件路径，由调用方创建输出。最后不足阈值的内容须显式`flush()`或`finish()`；析构不隐式吞掉写出错误。`finish()`在成功后幂等，并禁止继续观察；若计划从LimitReached继续，只flush，等最终停止再finish。

写入或flush失败立即抛异常并将sink标记为失败，保留未确认写出的缓冲供诊断；不能继续重试写入而声称文件无重复，因为底层输出可能已写出部分字节。当前没有落盘fsync、崩溃恢复、压缩、文件轮转或后台I/O线程。文件I/O消耗程序墙钟时间，不增加模拟Tick。

## 格式与关联

每行是完整JSON对象，含`schema_version:1`、从1递增的`sequence`和`type`。sequence是观察次序，不是额外事件、模拟Tick或跨运行ID；同Tick严格保序。定义类记录可包含未来的`release`，不能把所有时间字段直接排序后替代观察顺序。没有发生的里程碑不会被补造；可选字段为null。

| 记录类型 | 关键内容与用途 |
|---|---|
| `GlobalDefined / GlobalReleased / GlobalFinished` | Global id、op、bytes、预定release、实际释放/结果时刻；success和reason是逻辑工作结果。 |
| `InputRegistered` | 仅显式提供offered_at时输出；含id、offered、registered。计算外部输入等待，不改变原有延迟口径。 |
| `WorkDefined / WorkFinished` | 稳定Work id、parent Global、channel/local_byte/content和最终结果；每个Work代表64B原始逻辑工作。 |
| `AttemptStarted / AttemptLinked` | 尝试id、command、地址、大小、op、读提示/组、unit、入队时刻、unit_attempt、replay_pass和原Work关联。 |
| `AttemptAdvanced / AttemptDelivered / AttemptAssessed` | RX/正式接受、响应交付、Host核验；保留op与status，读写相同数值状态不等价。 |
| `PendingInitialized` | channel的Pending上限和观察起点；纯读运行也有零占用基线。 |
| `DluObserved` | 真实Assembly change、channel/dlu/generation、phase、received_fragments、first_arrival、deadline，以及可选command/status和additional_info。 |
| `BankOperationStarted / BankOperationFinished` | 稳定ticket、物理channel/die/bank/block/page/byte_offset、kind和实际起止时间。 |
| `ReadWindowEntered / Removed / Released` | 尝试身份、成员、channel、释放原因、窗口first_arrival/deadline和实际释放时刻。 |
| `ReadServed` | Cache或sense的实际服务时刻，sense可关联bank_ticket。 |
| `StreamEnd` | 导出结束时刻at和stop_reason；结束文件不等于所有请求成功或所有活动排空。 |

数值枚举属于schema 1：op为Read=0、Write=1；AttemptAdvanced.stage为RxArrival=0、Accepted=1；read_hint为Regular=0、Batch=1；WritePhase为Collecting=0、Ready=1、ProgramQueued=2、Programming=3；AssemblyChange为Opened=0、Progress=1、PhaseChanged=2、Retired=3；Bank kind为Read/sense=0、Program=1、Erase=2；ReadServiceKind为Cache=0、Sense=1；窗口reason为Batch=0、Timer=1、Threshold=2、Immediate=3、Policy=4。更改这些语义必须调整schema或保持明确兼容关系。

普通AttemptLinked直接给出原Work IDs；replay的work可为空，消费者按AttemptStarted的channel/local range和AttemptLinked的content查回原写Work。replay不创造新的逻辑统计分母。DLU用`(channel,dlu,generation)`关联，Bank用ticket关联，不能因文件flush或同地址复用而重新编号。

## 离线重建与内存边界

消费者按sequence读取并跨flush保留未完成关联：

- Pending曲线从初始化零值开始，Opened加一，phase变化在分区之间转移，Retired减一；Ready和ProgramQueued均计入ready_waiting。始终满足total=collecting+ready_waiting+programming。
- 阶段时间用真实Opened/Ready/Programming/Retired事件计算；收到StreamEnd时仍活跃的上下文只积分到观察终点，不虚构Retired。
- 请求延迟用同一Global/Work/Attempt的起止记录计算；汇总多个文件片段不能直接平均各片段的P99。
- Bank占用从每个ticket的起止事件重建；中断时仍在途的操作保留开放尾段。

[CLI验证](../tests/check_event_stream.py)把97字节与65536字节阈值的输出逐字节比较，并与完整Collector快照对照：普通运行、replay及运行限额场景的尝试关联、Global结果、Pending积分和Bank区间一致。它是验证消费者，不是通用JSONL到完整统计报告转换器；现有实验脚本仍读取schema 2完整JSON。

默认阈值65536字节是工程选择，与硬件缓冲无关。阈值限制的是待输出的序列化缓冲；单条超过阈值的记录完整直写，临时内存至少需要容纳这一条记录，底层ostream也可有自己的缓冲。`buffered_bytes()`、`peak_buffered_bytes()`和`records()`用于诊断该sink，不代表进程总内存。文件总大小随记录数增长。

长期运行可只启用事件流。`StatisticsFanout`允许同时连接事件流和Collector，适合交叉验证；它不拥有子sink，同时启用Collector仍会保存完整历史。Host的Global结果、内容覆盖/身份和恢复记账也是独立的正确性状态，不随统计flush清空。
