# GPU 实验设计 v2：基于 incremental-host-72e0745

日期：2026-10-31。状态：**待用户审查，仅设计，未实施、未运行实验。** 本版取代本聊天上一版方案。

依据：当前 README、INCREMENTAL_BACKEND、ARCHITECTURE、VALIDATION、DEPENDENCIES、实际接口和构建入口，以及用户提供的 `D:\HBF-Sim\gpu_experiment_handoff.md`。外部交接文档与当前 bundled 同名文件的 SHA256 相同：`efcb30843271e3ac653f55945633e91a1f4e0add5c7dc3a2bc88b7ac5485e59a`。本文采用其实验与验收要求，不采用人员分工或旧外部执行器的部署安排。

## 1. 更新后的判断

本工作区已具备完整增量后端，不再需要移植分支、补写 Host 或重建桥接基础设施。

| 项目 | 本次核实 | 对实验设计的影响 |
|---|---|---|
| 后端版本 | 72e074508c462dceb7d42962c4885ecf17cc4d0d，加已记录的六文件补丁 | 冻结现有版本及 overlay；主实验不修改核心 Host/Device |
| 整机路径 | CUDA/分页 KV → LiveTimingService → TimingBackend → SimulationSession → 原 HostDriver → 原 DeviceDriver | 上版“直接提交 Device”的判断已过时，删除相关整改工作 |
| 同步只读桥 | TensorAccessBridge 已有 initialize/access/advance_compute/finish、trace 和统计 sink | 直接用于交接文档要求的同步权重实验 |
| 共享异步路径 | TimingBackend 是 Session producer，支持多个父访问和可变 KV；不逐次调用同步桥 | 这是合理的现有适配，不要求把所有产品访问强行改走只读桥 |
| 容量/恢复 | resident_fragments=256，单位为活动 64B 逻辑片段；无前端 Device 命令窗口；Host 拥有重试和恢复 | 不引入 child_window，不把 GPU frames 与 resident 混用 |
| 时间 | 当前产品仍采用 GPU timestamp 驱动；README 明确未实现实测计算记录和 TTFT/TPOT 实验 | 仍需新增计算表、语义边界、严格回放与指标分析 |
| 执行器 | 当前 llama.cpp/F16、norm/down 选区、批量 prefill、多 token 已存在 | 使用当前执行器；不恢复旧 Small_case/FP32，也不照搬 23+1/24 位置假设 |
| 现有验证 | 发布记录载明 716 核心测试、18 算子、5 Python 验证及两个三模式模型案例 | 可作为基线来源，不能写成本轮执行结果；不能替代独立 CPU 全模型多 token 对照 |

仍待补齐的是实验层：多区域清单、事件编号、核验/计时模式、GPU 段测量、固定计算表、token 时刻、EOS/0/1 输出处理、独立 CPU 参照、指标与独立审计。

## 2. 推荐方案与实验边界

首轮分为两个明确命名的实验系列。**主图来自 S 系列；A 系列验证当前产品路径，不与 S 系列拼接成一个时钟。**

| 系列 | 路径与目的 | 计时/解释 |
|---|---|---|
| S：同步权重门控 | 当前 llama.cpp CUDA + 原生 F16 KV → 实验适配器 → 现成 TensorAccessBridge → Session/Host/Device | 固定 GPU 计算表 + 串行选区等待；完成原交接的 G1/G2/G3 与三张主图 |
| A：现有整机验证 | 原 native/paged bypass/modeled，保留 PTX/warp 访问、分页 KV、有限 frames、共享 Session | 报当前在线时间策略、真实 token 墙钟与完整功能审计；不能套用同步时间相加公式 |

S 系列选择只读桥是因为交接文档要求同步、无重叠、固定计算输入，且它已提供所需接口。A 系列的异步可变共享 Session 不需要整改。首轮不把 KV 写回、预取、热模型和文件 backing 加入 S 系列，避免把这些费用解释成权重等待；它们仍是工作区已有功能，不是被判为不支持。

S 系列权重仍在 GPU 显存；HBF 初始化和读取的是符号内容。它只能说明该同步门控模型中选定范围的等待影响，不证明真实 NAND 字节搬运、权重显存节约或校准后的硬件吞吐。

## 3. 输入、映射和工作量

### 固定输入

- 模型：Qwen3-0.6B F16 GGUF，F32 norm、F16 KV；由当前 profile generator 检查实际元数据、GGUF 与 conversion 哈希。不凭模型名称推定形状。
- prompt：现有 `adapters/llama_cpp/prompt.txt` 原始 UTF-8，冻结实际 token IDs；不隐式套 chat template。
- 单请求、单序列、greedy，最多 8 个非 EOS 输出；context=512、batch=256、ubatch=256，沿用当前批量 prefill。P<=256 且 P+8<=512，否则输入前置失败。
- CUDA graphs/flash attention 关闭，线程和精度固定。S 系列在门控边界显式同步依赖，禁止跨门控边界重叠；同一 kernel 内的 CUDA 并行保留。
- 模型、GPU、工具链和本地 profile 待审批后核实；此次只读检查不证明它们已就绪。缺输入就报告缺项，不自动换模型或 CPU 降级。

### 清单

令 H=hidden、F=ffn、L=min(10,layers-2)。三条主/回归清单使用同一 CUDA 算子和全量事件序列，只改变过滤范围。

| 清单 | 有效源范围 | 地位 |
|---|---|---|
| M8 | `blk.L.ffn_down.weight` offset=0、bytes=8192，确认在 tensor 内 | 原 8KiB 语义回归；不声称重现旧 q_proj 实验 |
| M2 | `blk.L.ffn_norm.weight` [0,4H)；`blk.L.ffn_down.weight` [0,2F) | 主实验，多 tensor，沿用现有选区含义 |
| M2-offset | norm 同 M2；down 从 offset=64 取 2F bytes，确保范围合法 | 非零偏移/跨行回归，覆盖量不变 |
| M-wide | norm 同 M2；down [0,4F) | 可选覆盖敏感性，固定资源预算 |

每条记录包含 region_id、tensor、storage_id、source_offset、valid_bytes、global_base、registered_bytes、content_id、canonical_region、aliases、padding。注册长度向上补到 4KiB，Global 基址顺序分配；偏移与有效长度遵守 64B 对齐。填充明确初始化，不计有效权重覆盖。同一逻辑 block 所有区域统一 content=1；别名指向主区域，不重复注册。未声明的重叠与越界拒绝。

S 系列访问粒度是“每个执行位置对某权重算子输入范围的一次逻辑读取”。prefill 的一个 CUDA 算子处理 R 行时，先产生 R 条带 position 的逻辑访问，并按 position/region 固定顺序服务，再发射该算子。时间表按实际 kernel/计算段测量，不凭 R 把 kernel 时间乘 R。该策略是明确的保守批量门控假设，不是对每次物理 load 的测量；现有 PTX 4B、warp 128B 请求计数仅属于 A 系列。

所有相关权重输入事件在过滤前稳定编号；一个回调命中多个区域，计算推进一次，交集逐个串行完成。hook 必须位于消费者实际 CUDA 发射之前；图构建 metadata 回调本身不保证执行边界，不能直接当作“已访存”。发生失败时不发射消费者。

覆盖分别统计：去重注册有效字节、触达区间并集、累计有效访问字节、填充、Global 流量、64B 命令流量。例：H=1024/F=3072 时 M2 有效10240B、注册12288B；这只是形状示例，实际值从 GGUF 得到。无 EOS 且输出 N>0 时执行位置 P+N-1，对选中非末层预期逻辑流量 `(4H+2F)*(P+N-1)`；审计仍以实际事件重建，不能直接套公式作为证据。EOS 检测步另记，可能增加执行位置但不增加输出 N。

## 4. 冻结后端配置

S 系列采用 `tests/incremental_bridge.cpp::config()` 的已存在配置作为起点：geometry={1,1,2,16,64,1}、capabilities={256,4,false,true}、MOCS=256、timeout=10000000ps、Burst4096、AllRegular、resident_fragments=256、FCFS Host/NAND，Global chunk=4096B。所有值经公开 resolver 校验，完整有效 INI 随结果保存。

介质参数沿用当前源码示例：sense=100000ps、program=500000ps、erase=1000000ps、cache=100ps；read window=10000ps、threshold=32；Grade1 链路、控制等效字节各16B、请求/响应固定延迟各100ps。ingress=16/16384B，egress=8/512B。**这些是未校准模型参数。** 不为得到明显差异擅自调大它们。

每个配置从新的 Session 开始；通过 initialize 普通 Global 写按合法页序完成全部注册范围。逐次检查 success，初始化耗时和 program/erase 单列；t0 在初始化之后。保留合法初始化后的设备状态，不用私有接口“清 cache”。所有覆盖清单使用同一设备几何与预算，容量不足直接报告。

S 系列拟定请求级 events ceiling=10^7、同 Tick/round guard 各100000、无有限模拟时间上限；单进程墙钟限额3600s。故障/限额测试用显式独立配置。预算变化产生新的配置版本，不能静默扩容或把超限混进成功样本。A 系列继续使用现有 profile 派生预算，不用 S 的紧凑几何替换全层 KV 配置。

## 5. 三种时间和计算表

### 原生真实测量

S-native 与 S-HBF 使用相同 CUDA 算子拆分、精度、原生 KV 和事件边界；HBF 的开关只控制依赖等待。预热3次完整请求，正式采样10次，每次重置请求/KV。模型加载和符号初始化在请求计时外。

在各门控边界之间用 CUDA events 测 GPU 工作，保留首个访问前、未选中回调之间、prefill 边界、每 token 尾段、EOS 与结束剩余段。必要的数据传输与 kernel 分列。event 区间不能跨后端调用、CPU 等待、核验快照或文件输出；不能把 modeled 路径里自旋等待的 kernel 时长当作纯计算。

另做10次不启用细粒度 event/快照的 native 请求测量，保存单调主机时钟的请求与 token 可交付时刻，评估插桩成本。真正必要的 logits 取回/采样仍保留；关闭的是额外核验导出。

### 固定计算输入

保存10次完整原始表；主表取每段中位数，并明确它是组合代表表，不是某一次请求。G2/G3 使用完全同一表。补充用10张逐次原始表进行配对回放，展示计算波动，不宣称8个 token 足够估计 p99。

段键为 request/phase/execution-position或position-range/op-occurrence/segment ID；不能只按 tensor 名索引。header 绑定模型、prompt IDs、生成配置、算子/精度、源码、二进制与事件序列哈希。清单变化只允许筛选同一事件序列；改变 batch、精度、CUDA 算法或输入必须重测。

CUDA event ms ×10^9 转整数 ps，round-to-nearest，检查非负/有限/溢出；记录分辨率，接近分辨率的段标记，不人为加 Tick。可合并不跨访问/token 边界的段，禁止为得到非零数越界合并。

执行顺序：`advance_compute → access各交集并检查最终成功 → 后续计算 → token事件`。access 的 compute 参数设0，避免已经推进的计算再次计费。零段沿用同 Tick 续体；首段与每 token 尾段均只计一次。缺段、错序、重复、哈希不符立即拒绝，不能自动退回固定1000ps。

### 指标定义

- t0：已加载、已初始化、已分词后，开始请求推理。
- prefill_finished：prompt GPU 工作完成；token_emitted：采样已确定可交付 token ID，位于文本解码/打印之前。二者语义不同。
- TTFT=t1-t0；ITL_i=ti-t(i-1)；TPOT=(tN-t1)/(N-1)。N=0 的 TTFT、N<2 的 TPOT 为 null。
- EOS 不计输出 N；保存 raw EOS token 与独立结束事件。输出上限后不执行预测下一枚 token 的额外工作。当前 runner 已无该多算问题，保持它，而不是照搬旧24位置要求。
- 每个 S 模拟 token 区间必须满足：总长=GPU计算+纳入模型的GPU传输+HBF等待，整数 ps 残差为0。
- CPU sampling/调度等未建模部分明确列出；真实 native 请求墙钟与 modeled 组成不同，不能直接用二者相除声称 slowdown。
- 加载、初始化、核验导出、后端运行墙钟、最终排空另列。EOS 检测尾段可计入请求结束，但不能回加到最后一个用户 token 的时刻。

## 6. 实验矩阵和停止条件

| 编号 | 内容 | 数量 | 验收/用途 |
|---|---|---|---|
| G0-contract | 现有 incremental/tensor/session/compute 回归，加第8节契约案例 | 每例1次 | 复用已实现基础；关键同 Tick/计时例重复3次 |
| G0-GPU | S 的 M8、非零偏移、零计算间隔、依赖失败/限额 | 每例1次 | 接入行为与失败不继续计算 |
| G1 | S-native，固定短 prompt/最多8输出 | 3预热+10段测量；10无细粒度event墙钟 | 完整计算表与真实请求参考 |
| G2 | 相同真实CUDA执行/同表/同事件，HBF等待为0 | 中位表3次 | modeled计算对照；不拿旧bypass墙钟替代 |
| G3 | S-HBF，M2，同输入/同表，经同步桥 | 中位表3次 | 主结果；逐访问成功、逐token守恒、最终排空 |
| N | 独立CPU/native/G3，全量核验；计时模式一致性 | 主案例各1次；GPU计时模式各1次 | token、全logits、关键状态正确 |
| V | 10张原始计算表，各做G2/G3离线配对重放 | 10对 | 计算测量波动；注明重放，不能替代G3真实门控验收 |
| A | 当前产品三模式的 Qwen short/32、Smol long/64 | 各1完整审计 | 整机集成回归，另报在线/墙钟指标，不入同步主图 |
| G4可选 | M-wide；sense倍率0.5/1/2；每次只改一项 | 每配置1次 | 覆盖和参数敏感性，主验收完成后开展 |

G2/G3中位表3次的模拟时刻必须完全相同；差异先作为确定性缺陷处理，不计算“平均后看起来一致”。真实计算样本可有波动。

任何数值失败、时间表错配、未解释残差、审计丢记录均停止相应结果进入图表。正常 EOS 保留实际 N；若 N<2，报告 TPOT 不适用，再追加预先登记的备用 prompt 以满足多 token 展示，不能隐瞒原案例或强行禁用 EOS。

## 7. 数值核验与拟修改范围

现有 native/bypass/modeled 并不等于独立 CPU/native/HBF；新增 CPU 全模型参照仍是原文验收项。建议主模型用独立 NumPy 数学实现读取同一 GGUF，复用输入 token IDs，明确 Qwen3 RoPE、QK norm、GQA、F16 KV 舍入与 greedy/EOS，独立于被测 CUDA 算子。优先复用可验证的现有 CPU 数学参照组件，但不能直接把旧 FP32 Small_case 权重/阈值用于新 F16 路径，也不把运行同一 CUDA 实现的 bypass 当独立参照。

预声明验收：每步完整 logits 对 CPU 的 max_abs<=0.2、RMS<=0.025、tokens_exact；这是沿用当前 Qwen profile 上界作为本轮提案，不是已证明的 CPU 精度。hidden/KV 逐元素拟用 atol=0.002、rtol=0.005，并报告各层 max/RMS 与超阈比例。若不通过先查舍入与算子差异；不按结果自动放宽。CPU 精度契约若确需修订，应单独形成审查记录后重跑。

S-native/G2/G3 相同 CUDA 算法与顺序下，逐 token logits 和关键快照要求逐字节相同；verify/timing 模式 token 和最终 logits 也须一致。核验模式保存每位置选中层前后 hidden、各层新 K/V、全部输出 logits；计时模式关闭额外 D2H 快照与磁盘导出。CPU/native/G3 不能只比较最后一个 token。

拟修改集中在当前 integrated runner/weights/profile、实验适配/测量/分析脚本及对应测试。链接现成 `HBFEventLab::tensor_bridge`；不新增 Host/Device，不改其调度、内容身份、重试或时钟。新增应用语义放实验层，token 不进入核心。通用配置/IO/来源记录优先复用 `third_party/HBF-EventLab/experiments/common/` 与当前 build identity 工具。

现有 runner 仍限制输出2..448、早期EOG抛错、process_wall_ms在采样前结束；新实验入口需支持0/1/正常EOS及可交付时刻。建议独立实验模式或 runner，保留旧验收入口的严格固定长度行为。

## 8. 必须通过的契约审计

1. 无 CUDA 时间例：10μs计算+20μs等待+5μs计算产生token1，TTFT=35μs；再3+7+2μs产生token2，ITL/TPOT=12μs。
2. 首段、末段、未选中回调、多区域同回调、prefill批量段、N=0/1、EOS、零计算、表缺失/重复/错序/输入不匹配。
3. 多区域、非零offset、尾填充、别名去重、跨区交集、非法重叠/越界、容量不足、未选区不提交。审计从原始回调和清单独立求交，验证源偏移→区域偏移→Global，不照抄适配器汇总。
4. 合法完整DLU初始化，同block同content；所有Global final结果归属明确。父完成时刻=所有子Global最终完成最大值，完成与成功分别检查。
5. 容量背压保留未登记范围，通知后重查；命令尝试失败不直接等于Global失败；Host retry/replay仍由原Host处理。
6. 最终依赖失败立即禁止消费者发射。正常失败停止未登记后缀并排空已登记工作；严重Session错误/限额保留未决项，不能宣称已排空。
7. `finish`关闭来源并验证Host/Device排空；A系列还应区分end_stream的临时排空。初始化program与应用writeback分开，logical snapshot pages与host_replay_commands分开。
8. full audit/lean timing两种观察模式结果一致。精简模式不声称做了完整事件审计；CUPTI dropped、文件缺失、日志截断使完整审计无效。

实施顺序：复用桥契约确认 → 新事件及合成计时 → 单选区真实CUDA → 多区域/多token/CPU核验 → G1测量冻结 → G2/G3 → 图表 → 可选敏感性。阶段失败不越过验收继续解释性能。

## 9. 记录与交付

统一实验 schema_version=1。新增文件/工具均为拟实现，当前不声称已经存在。

| 文件 | 内容 |
|---|---|
| provenance.json / effective.ini | 后端提交+六文件overlay、框架和实验补丁、构建/二进制哈希、GPU/驱动/CUDA、模型/conversion/prompt/配置/表哈希、运行模式 |
| regions.json | 多区域/别名/填充/Global与content布局 |
| events.jsonl | 稳定request/event/callback/phase/position-range/op-occurrence，包括未选中事件 |
| compute-samples.jsonl / compute-table.json | 原始GPU样本、单位/分辨率/舍入、代表值、事件绑定 |
| requests.jsonl / tokens.jsonl | t0、prefill、各token真实与模拟时刻、raw EOS、实际N与结束原因 |
| accesses.jsonl / globals.jsonl / statistics.jsonl | 回调交集、桥父子归属、登记与最终结果、容量、尝试与恢复 |
| segments.csv / metrics.csv / coverage.csv | 分段compute/transfer/wait、TTFT/TPOT/ITL、整数残差、三类覆盖与流量 |
| numerical.json / audit.json / report.md | 误差、精确比较、独立行为审计、失败与未决项、结论边界 |

主交付三张图，各附PNG/SVG/原始CSV：

1. G2/G3 modeled TTFT与平均TPOT对比，N<2标N/A；native真实指标放独立面板，注明时钟组成。
2. token 2..N 的逐token ITL配对曲线，不宣称可靠尾分位数。
3. prefill/decode 的GPU计算、必要GPU传输、HBF等待堆叠，附逐token分解与残差。

另附覆盖表、数值误差表、状态/失败表、初始化/后端/联合程序墙钟表、A系列独立整机结果，以及可复现脚本与所有产物SHA256索引。原始失败样本保留，不混入成功延迟统计。大型权重/快照/trace不纳入源码包。

拟提供 `tools/run_gpu_experiment.py` 的 prepare/verify/measure/replay/analyze 阶段入口；schema/参数在实施时冻结。现有 `run_llama_integrated.py` 用于A系列，不能直接产出本设计的S系列主图。

## 10. 构建与审查范围

当前模型构建具有位置绑定、全源码清单封存检查。实施时按README要求在工作区下创建新的实验源码快照和构建目录，重新生成合法identity；不覆盖现有seal、不删校验、不把旧发布二进制冒充新实验代码。保存发布基准与实验差异。此次只新增docs方案，不改源代码或已有验收记录。

建议审查通过的范围是：**复用现有桥和增量Host；当前llama.cpp/F16执行器；M2双tensor、短prompt、最多8输出；保留批量prefill并声明串行输入门控假设；独立CPU多token对照；固定GPU表的G1/G2/G3及三图；现有异步分页KV作为独立整机回归。**

与上一版相比，取消了Host移植/新增Session/桥实现、外部后端查找、以及强制ubatch=1。新增重点是明确现成同步桥与共享异步producer各自的用途，保留当前产品能力与构建身份约束。

**用户审查前不实施、不构建、不运行测试、推理或计时。**
