# Tensor 访存桥接

`TensorAccessBridge` 将执行器的 `(tensor, offset, bytes)` 回调变成现有 Host 的增量 Global 输入，再按 Global 的最终逻辑结果完成父访存。它是软件适配层，不是另一套 Host，也不直接向 Device 提交请求。默认 CPU 示例无需 CUDA、GPU 或模型权重；可选的[Qwen 在线实验](qwen_online.md)复用同一桥接层进行真实 CUDA 推理验证。

## 构建和最小验证

```sh
cmake -S . -B build
cmake --build build --config Debug --target hbf_eventlab_tensor_bridge_demo
ctest --test-dir build -C Debug -R '^(session_handoff_|tensor_bridge_)' --output-on-failure
```

Windows 多配置构建运行 `build/Debug/hbf_eventlab_tensor_bridge_demo.exe`；Linux 等单配置构建运行 `build/hbf_eventlab_tensor_bridge_demo`。对外链接目标为 `HBFEventLab::tensor_bridge`，头文件为 [tensor_access_bridge.hpp](../include/hbf_eventlab/adapters/tensor_access_bridge.hpp)。它依赖 `HBFEventLab::core`，核心库不依赖该适配器。

[示例](../examples/cpu_tensor_bridge.cpp)使用一个 channel、64个驻留逻辑片段预算、8KiB权重区和4KiB Global子范围：

1. 通过普通写路径初始化完整权重区；第二个Global暂时因驻留预算不足而等待。
2. 增加250000 Tick的模拟计算间隔，再读取整个权重区。
3. 收到最终反馈后，以零计算间隔读取64B，通过同Tick后续事件登记。
4. 关闭来源，检查Host与Device排空。

输出包括 `input-backpressured`、`global-registered`、`global-finished`、`access-finished` 和最终 `CPU_BRIDGE status=Completed success=1`。Tick单位沿用配置契约（当前1ps）；这些固定时延用于功能验证，未经性能校准。CPU示例的数值求和只说明执行器仍拥有自己的真实数组，并非NAND字节传输或Qwen数值正确性的证明。

## 调用契约

```cpp
SimulationConfiguration config;
config.device.geometry.host_channels = 1;
config.host.max_resident_fragments = 64;

TensorAccessBridge bridge(resolve_configuration(config),
    {{"weights", GlobalByteAddress{0}, 8192, 1}});
auto initialized = bridge.initialize("weights");
// 必须检查 initialized.success，失败则不能继续执行依赖该数据的计算。
auto read = bridge.access("weights", 0, 8192, Tick{250000});
// 必须检查 read.success，再使用执行器自己持有的真实字节。
auto tail = bridge.advance_compute(Tick{5000});
// 最后一次访存后的显式计算尾段也须检查 success。
auto drained = bridge.finish();
// 检查 drained.run.status / workload_success 和每次访问的 success。
```

| 对象／调用 | 语义 |
|---|---|
| `TensorRegion` | 名称、Global字节基址、长度及不可变content。范围需64B对齐、不重叠、位于设备逻辑容量内；同逻辑块的多个tensor必须使用相同content。使用静态逻辑地址视图检查，不查询动态物理映射。 |
| `initialize(name)` | 每个注册tensor最多尝试一次普通写初始化；不直接修改NAND状态、不制造额外补片。支持范围应覆盖完整local DLU，并满足前页条件。 |
| `access(name, offset, bytes, compute)` | 同步的只读访存边界；先推进固定计算间隔，再提交范围，直至该父访问有最终结果或无法继续。未成功时不得继续依赖其数据的计算。 |
| `global_chunk_bytes` | 默认4096；正的64B倍数，仅决定父访存拆成几个Global。Host仍按原配置决定64B命令、burst、发送策略、MOCS和Pending。它不是并发命令数量窗口。 |
| `TensorAccessResult` | 父访问ID、产生/完成时刻、逻辑成功、已登记/终结Global数、真实容量拒绝次数、未登记字节数和停止原因。`complete`与`success`分开；输入契约错误也不能被视为成功。 |
| `advance_compute(duration)` | 独立推进计算区间，返回起止Tick、success、停止原因；可用于未选择的执行器访存间隔和最后的计算尾段，不创建虚拟内存访问。0保留同Tick反馈资格，溢出先拒绝，运行限额不能报告为成功。 |
| `finish()` | 关闭来源，未登记后缀不再提交，按既有限额排空已登记工作。返回后端SessionReport；它只涵盖已经登记的工作，不能代替每次父访问的成功检查。 |

首个Global从1开始递增，父访问也使用独立递增ID；`set_trace()`可记录两者归属。同一个父访问包含任意多个Global，完成时间取这些Global最终结果的最大时刻；Host内部可重试或replay，桥接层不把一次命令失败当作最终逻辑失败。

首次容量拒绝后，适配器保留下一个Global的地址和偏移。容量回调仅表示进展；空闲预算足以容纳整个子范围时才重新提交，不因每释放一个64B片段就盲目重试。每个Global保留父访问最初的 `offered_at`，实际登记时使用合法的当前时刻。无额外 `child_window`、自动缩小子范围或补片预留。

若任一Global最终失败，停止未登记后缀，并等所有已登记子Global终结后报告失败。若Host拒绝后续输入（例如累计输入保护或已中止的内容版本），保留输入契约诊断，继续处理已登记工作，不伪造Device错误响应。`WaitingForInput`、`LimitReached` 和模型/实现错误均不能当作成功；适配器停止接受新的父访问。严重会话错误后应保留诊断并终止该会话，不承诺可继续排空。

## 时间协调与因果交接

单生产者、同一模拟线程、同步访存边界。一次访存完成于T，下一段计算耗时Δ，则下一访存的产生时间为 `T + Δ`。计算间隔内仍处理已有HBF事件，但不会受旧容量通知影响而提前登记下一访存。模拟器执行自身消耗的真实时间不进入这个公式。

`SimulationSession`提供以下机制：

- `request_yield()`：仅在会话反馈回调中提出正常交接；当前事件安全结束后返回 `Paused`、`feedback_yield=true`。不会额外执行未来事件或重置安全预算。
- `advance_until_yield()`：生产者承诺当前没有更早的独立外部输入，再推进至所请求的交接；无事件且来源开放时返回 `WaitingForInput`，保持当前模拟时间。
- `resume_feedback(callback)`：一次性交接续体。排入当前Tick的正常Arrival事件，在后续因果轮次执行；此回调可调用 `try_submit_feedback()`。用于Δ=0，不虚加Tick，也不重开普通外部提交入口。再次推进会作废尚未使用的续体资格。
- Δ>0使用 `advance_before(T+Δ)`，随后在尚未执行的排他终点提交。Δ=0使用续体；两者均不向过去回填。

冻结的deadline批次仍由EventQueue整体结算，交接不会把它拆开。timeout后的续体不能补救已经终结的旧上下文。关闭外部来源后，已取得的因果续体仍可产生工作；完全关闭应用时应停止生成续体并完成排空。

## 数据边界与当前限制

执行器持有真实权重字节并负责数值计算；后端只保存既有逻辑内容身份和DataToken，负责行为、时序与完成验证。初始化与读都沿用当前block内容身份，不采用“每个页一个新版本”的替代规则。重放仍由原Host根据其保留区处理。

当前适配器用于固定、只读权重区域的初始化和读取：不提供更新tensor内容、KV写回、异步多GPU、多线程或GPU/HBM一致性协议。它不动态装入真实数据，也不读取Device私有状态来补齐数据。

跨channel地址交织仍由原Host完成。Global 4KiB不一定是完整local DLU；严格驻留预算、Global分组和初始化覆盖需共同满足前进性。例如两个channel、64B交织时，一份8KiB完整覆盖可形成两个local DLU；若预算只容纳最初的64个逻辑片段，后续补片可能无法登记而超时。适配器如实暴露这一结果，不悄悄扩容。显式burst还要求每次提交已覆盖完整local DLU。

可选Qwen接入复用名称/offset/bytes回调，采用每次回调间固定计算时延和显式尾段，细节见[实验说明](qwen_online.md)。若未来采用测得的GPU计算耗时，应只测计算区间，排除Host模拟推进、统计导出及访存等待时间。
