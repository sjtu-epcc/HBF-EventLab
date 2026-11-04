# Qwen 选区规模与模拟器墙钟测量

现可在HBF实验命令后加入`--detail off --profile on`，单独导出`<output-prefix>.profile.json`，进一步分解准备／拆分／登记、发送、状态推进、results扫描及回收的程序耗时；见[程序性能诊断](runtime_profiling.md)。原有墙钟样本不含这些新计时项，不能当作已经测得函数占比的证据。

8KiB接入检查用于验证流程，不能证明更大工作集上的运行效率。本实验保持原Host和既有内容身份规则，扩大所选权重范围，并在同一机器运行组员的原始实现作参照。默认只做单次诊断样本，不形成稳定性能排名。

## 原实验的范围

复核对象为[Small_case](https://github.com/ZeePgm/Small_case/tree/3d8a58ba1c321f41edc3280acda70388964a39c9)的固定提交：

- `src/qwen_run.cpp`中`Bridge`选择第一层`q_proj.weight`的完整8MiB，初始化2048页。`timing-selected`在每个执行位置详细读取这2048页，等待所有响应后才继续CUDA计算。
- 它不对全部权重执行详细事务。`timing-aggregate`虽然对更多访存累计成本，但只是使用校准出的页成本记账，不逐命令调用后端，也不按估算值等待。
- `src/scale_gate.cpp`另有8MiB校准和32MiB留出检查，每个工作集读取两遍。这是后端规模实验，不是全部Qwen权重详细映射。
- 其[开销报告](https://github.com/ZeePgm/Small_case/blob/3d8a58ba1c321f41edc3280acda70388964a39c9/docs/results/overhead-2026-10-22/REPORT.md)在另一台机器上测得：8MiB读两遍，Online中位1.650秒、Direct中位1.436秒。移除线程交接约节省12.9%，事务推进仍占主要部分。这个结果不等于当前机器或当前Host的性能。

固定23-token prompt、生成1 token时共有24个执行位置。8MiB选区每位置产生131072个64B读，总计3145728个读命令。当前fragment64初始化还产生131072个写命令，共3276800个命令尝试（无故障时）。组员以2048条4KiB写初始化，两者的初始化命令数量不同。

## 测量边界

1. 整个子进程墙钟：外部Python计时，包含程序启动、初始化、GPU模型加载、推理、输出及退出。
2. `initialization_wall_seconds`：当前桥接的普通Host符号写初始化。
3. `inference_wall_seconds`：实际推理循环；包含CUDA、执行器/快照和后端推进。
4. `inference_backend_wall_seconds`：推理期间在桥接访问及显式计算推进中花费的真实时间；包含Host、Device、事件处理及启用的observer开销，不是Device内部纯执行时间。
5. 模拟Tick：继续独立报告，不能代替上述程序运行时间。

组员结果中的`seconds`从模型加载和初始化之后开始，到快照关闭和runner结束为止。不能直接与整个子进程墙钟混用；脚本保留两种口径。

所有当前Host尺寸案例使用统一几何和资源预算：1 channel、1 die、2 banks、4096 pages/block、4 blocks/bank、MOCS=256、Pending=4、RX/TX各1项；64个活动逻辑片段预算、FCFS、fragment64写。累计输入及事件上限显式提高到可容纳本实验的数量，仍保留模拟时间与同Tick保护。有效配置随输出保存。

这些静态资源尽量对齐原实验，但两个执行流程不是等价实现：组员使用简化StreamingHost、两页父批次、child_window=4、每页独立version；当前使用原Host的block内容身份、Global结果、恢复/重试和回收契约。差值不能直接解释成一次接口改动的收益或某个函数的耗时。

详细日志对照只在8KiB上开启full/off两次；更大案例使用off，避免数百万命令产生的大体积JSONL混入主测量。所有案例仍有少量回调/进展输出。微型对照必须满足数值、模拟终点、业务计数和事件数一致；单次开关差值不能作为精确的日志开销占比。

## 复现

按[Qwen说明](qwen_online.md)准备模型和当前CUDA目标。组员源码作为外部只读输入，在本项目构建目录中另建它的Release可执行文件：

```powershell
$executor = '../Small_case'
$model = '../models/Qwen3-0.6B'
$python = '../qwen-env/Scripts/python.exe'
cmake -S $executor -B build-qwen/teammate -G 'Visual Studio 17 2022' -A x64 `
  -T cuda=12.9 -DCMAKE_CUDA_ARCHITECTURES=89
cmake --build build-qwen/teammate --config Release --target gpu_hbf_qwen

& $python experiments/qwen_online/measure_wall.py `
  --program build-qwen/experiments/qwen_online/Release/hbf_eventlab_qwen.exe `
  --teammate-program build-qwen/teammate/Release/gpu_hbf_qwen.exe `
  --model $model --reference-prefix build-qwen/validation/reference-1 `
  --output build-qwen/wall-measurement --wall-budget 300
```

`reference-prefix`来自同一模型和prompt已完成的独立CPU参照；脚本检查成功样本的生成结果、数值快照、选区覆盖和最终排空。默认尺寸为128KiB、1MiB、8MiB，另有8KiB full/off和两个原生GPU基线。可用`--sizes`调整尺寸；输出目录必须不存在。

运行严格串行，不同时执行构建或另一份性能测试。脚本每个样本结束后即保存`measurements.json`和`report.md`，并保留原始命令、返回码、日志、配置、模型/程序/脚本哈希。逐回调的`backend_wall_seconds`可观察相同大小访问随运行进度的成本变化。

`--wall-budget`是本次实验子进程的程序墙钟保护，和核心模拟器的`LimitReached`或DLU timeout不同。超限时停止该子进程，标记`wall_budget_reached`，保留最后已刷新进展；没有完成JSON或完整数值快照就不能算成功，也不能声称已排空。该记录只给出完成时间下界，不能用比例直接外推全权重性能。

## 后续定位方向

大案例的活动片段数受限，并不意味着每次推进只访问这些活动对象。本实验记录对应的旧实现通过`HostDriver::advance()`中的`std::all_of(results_)`检查所有Global是否结束；保留的成功摘要使扫描长度随历史增长，Host活动及成功查询也遍历摘要。后续实现已用未完成／最终失败计数替代这三处扫描，见[设计决策](design_decisions.md#adr-host-efficiency-01)；本文原有墙钟记录不因此改写为优化后的测量。

后续仍需测量回收、发送资格、恢复检查和重复输入准备。诊断方式见[程序性能诊断](runtime_profiling.md)。本次完成计数优化没有改变内容身份、依赖、恢复或恰好一次完成契约，也没有缩小历史表的内存占用；CPU基准收益不能直接当作真实Qwen或全权重实验的收益。
