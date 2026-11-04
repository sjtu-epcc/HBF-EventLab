# GPT-2 模拟 GPU + 增量 HBF：第 6 个 kernel 慢的诊断报告

日期：2026-11-03。供 HBF 后端及 GPU 接口维护者定位性能问题。这里的时间分为**模拟 GPU 时间**和**运行模拟器的 CPU 墙钟时间**；两者不能混用。

## 结论

更新分支 `8ccb1f353630f893f761b017d621812b7b9d5bf2` 没有卡死在第 6 个 kernel；四小时回放完成了 29/1480 个 kernel，随后按设定超时退出 124。第 6 个 CUTLASS FP32 GEMM 在 HBF 配置下需要 **30,999,754 个模拟周期**，相同轨迹、相同指令数在普通 HBM 对照下需要 **160,036 周期**，相差约 **194 倍**。因此，本次跑不完整个模型的首要原因是 **HBF 内存路径使 GPU 产生大量等待和反压，进而迫使逐周期模拟器执行远多于 HBM 对照的周期**。HBF 桥接本身另占短测 CPU 时间约 34%，加重实际运行耗时。

**已定位到具体接入代码机制：**GPU 页控制器只把同时在途的同页 sector 合并到一个 MSHR；`backend_completed()` 在页读完成时直接 `pages_.erase(page)`，没有保留已完成页面供后续 sector 复用。桥接层又把 `gc.mshrs` 固定为 32，当前运行的 HBF L2 策略为 bypass。因此连续到达的同页请求可以反复触发页级读和 NAND sense。同一 kernel 的 1,728 个不同页产生约 20,827 次页级读和 18,306 次物理 sense。**这首先是 GPU→HBF 页控制器/缓存模型的问题，不是已证明的新 HostDriver 逻辑故障。**是否应增加完成页缓存、采用何种容量和时序，须按目标 HBF 架构确定，不能随意消除真实 NAND 代价。

## 实验对象及边界

- 模型：预训练 GPT-2 124M；WikiText-2 实际测试文本。提示词 ` Robert Boulter is an English film`，8 个输入 token，最多 4 个贪心输出 token，FP32、batch 1、保留 KV cache。真实 GPU 参考输出 ID `[4014, 290, 6260, 13]`。
- 模拟：完整 1480 个 NVIDIA kernel 轨迹；权重地址映射到 HBF，激活和 KV 留在模拟 HBM。121,551 个权重页通过正常 Host/Device 写入初始化，未直接 seed NAND。真实 A6000 只用于采集轨迹和功能参考。
- HBF：16 通道，NAND `tR=15 µs`，UCIe 配置 188 GB/s；GPU 核心时钟 1410 MHz；HBF L2 默认 bypass，GPU 页控制器 32 个 MSHR，按 32B sector 请求，合并读开启。上述参数是实验配置，不是经过硬件校准的商业器件参数。
- 完整回放四小时退出 124，29/1480 kernel；所有输出 token 的 kernel 边界均未完成，因此**无有效模拟 TTFT/TPOT**。

## 可复核的对照

| 对照 | 第 6 个 kernel 累计结束周期 | 第 6 个 kernel 自身周期 | 前 6 个累计指令 | 120 秒完成 kernel |
|---|---:|---:|---:|---:|
| 增量 EventLab HBF | 31,117,840 | 30,999,754 | 94,677,930 | 5 |
| 同一模拟器、普通 HBM 内存模型 | 195,003 | 160,036 | 94,677,930 | 29 |
| 同一模拟器、旧内置 HBF 内存后端 | 未完成 | 未知 | 未到第 6 个结束 | 5 |
| EventLab HBF，GPU L2 开启 | 未完成 | 未知 | 未到第 6 个结束 | 5 |

普通 HBM 对照仅把 `-gpgpu_hbf_enabled` 改为 0，使用**同一可执行文件和指令轨迹**。它是定位等待来源的理想化内存对照，不能代表 HBF 的真实时序。旧内置 HBF 后端也使用同一 GPU 核心，不能算另一款 GPU 模拟器。L2 开启对照改变缓存策略，且只跑了 120 秒，不足以断言其长期效果。

第 6 个 kernel 从第 5 个结束周期 118,086 执行到 31,117,840。按 1410 MHz 折算，约为 **21.99 ms 模拟 GPU 时间**；普通 HBM 对照约 **0.114 ms**。这是模拟结果，不是实测 GPU 墙钟。

## HBF 请求、排队和反压

第 6 个 kernel 前，HBF 累计完成 7,680 个 32B sector 请求、66 次页级读和 21 次物理 sense。第 6 个结束时，累计为 228,864 个 sector 请求、20,893 次页级读和 18,327 次 sense。因此该 kernel 增量是：

- **221,184 个 32B 请求**，恰好覆盖 **1,728 个不同 4KiB 页 × 每页 128 个 sector**。全部请求均完成并返回，failed=0、owned=0，说明没有请求永久丢失。
- **20,827 次页级读、18,306 次 NAND sense**；分别约为每个不同页 12.1 次、10.6 次。GPU 页控制器虽记录约 200,357 次 MSHR 合并命中，仍存在大量重复页级操作。不能仅凭这些聚合计数判定每次 sense 的地址相同；应在后端增加按页的 sense/缓存命中统计验证。
- 从生命周期日志按请求 ID 匹配，第 6 个 kernel 的完成延迟 p50 **45.48 µs**、p90 **224.69 µs**、p99 **780.35 µs**、最大 **3.646 ms**。相比单次 NAND 读 tR=15 µs，长尾明显包含排队、重复读或其他路径等待；各项贡献尚未拆开。
- `gpu_stall_dramfull` 在该 kernel 增加 **214,959,424**；普通 HBM 对照同一 kernel 增加 **11,064**。代码中该计数是在每个 L2 子分区入口队列 `full(SECTOR_CHUNCK_SIZE)` 时递增，可能对多个分区和多个周期重复计数，**不是 2.15 亿个独立请求，也不是直接的 HBF `full()` 次数**。它证明 GPU 内存路径出现持续反压。

GPU/HBF 桥接的 `full()` 仅在 owned 请求数达到限额时返回真；L2 到 HBF 的入队路径另受 `can_issue_to_dram()`、本地队列和返回信用约束。现有计数不能区分反压源头。建议分别记录 L2 入口满、L2→HBF 信用不足、HBF `full()`、Host 输入容量不足、物理 NAND 忙、返回队列满的次数及持续模拟周期。

### 已核实的接入代码机制

用于本次服务器构建的 `adapter/src/gpu/page_controller.cpp` SHA-256 为 `b904a60cc9088e6086635b71fb93732a2cbce9c3a45688bb4c80ff88d501459e`，与本地同名源一致。`admit()` 在页仍存在于 `pages_` 时合并同页读并增加 `mshr_hits`（约第 99–107 行）；`backend_completed()` 取出等待者后无条件执行 `pages_.erase(page)`（第 157–163 行）。因此这是**在途合并**，没有完成页驻留缓存。桥接文件 `hbf_eventlab_bridge.cc` 第 116–120 行设置 `gc.mshrs=32`、`sector_requests=true`、合并读开启。当前配置 `hbf_l2_policy=0` 是 bypass；旧内置 HBF 的 `hbf_cache_entries` 不作用于 EventLab 页控制器。

第 6 个 kernel 的 221,184 个 32B 请求恰为 1,728 页 × 每页 128 个 sector；若每页只需一次后端页读，则为 1,728 次。实测 20,827 次页级读。这说明跨时间的页复用没有被当前在途 MSHR 合并充分捕获。因实际页缓存容量、替换及并发约束还未定义，不能把理想 1,728 次作为无条件正确的硬件结果。

## CPU 耗时说明

隔离的 120 秒直接 GPU+HBF 短测中，正常权重初始化后，`gpgpu_sim::cycle()` 的 101.35 秒 CPU 时间约分为：GPU 核心域 61.99 秒、HBF EventLab 桥接 34.55 秒、我们添加的 HBF 分区循环 0.55 秒、其他 DRAM 2.41 秒、L2 0.76 秒。轨迹块发射 1.36 秒包含在核心域中。新版 ResultSpace 广播读唤醒和重复 rearm 在这段短测中均为 0；旧问题已不再是这一阶段的主要耗时。Host 历史回收仍有约 11.08 亿次单位访问，值得独立优化。

**CPU 核心域占比高，不代表根因在计算指令。**HBF 对照使第 6 个 kernel 的模拟周期比 HBM 多约 194 倍，每个额外周期都会让 GPU 核心逐周期循环继续运行。这解释了为什么 CPU 时间主要落在 Accel-Sim 核心，而降低 HBF 内存等待仍可能显著减少总运行时间。

## 建议后端和接口维护者优先核查

1. **先由 GPU 接口与 HBF 后端共同确定完成页是否应驻留。**若目标架构有页缓冲/缓存，就在 `backend_completed()` 后按容量和替换策略保留读页，而不是直接删除；如果设计要求不驻留，则需确认当前 10.6 次 sense/页和约 22 ms/kernel 是否为预期性能。
2. 对第 6 个 kernel 的 1,728 页，按页输出 sector 请求数、逻辑页读数、实际 sense 数、page-buffer 命中/失效、再次 sense 的原因。验证 18,306 次 sense 是否符合设计预期。
3. 统计每个请求从 GPU L2 入队、Host 接收、排队、NAND sense、Device 完成、返回 GPU 的模拟时间分解，尤其是 p99 约 780 µs 的长尾。
4. 分别记录 HBF Host/Device 容量与 GPU 端本地队列/信用阻塞。不要用 `gpu_stall_dramfull` 直接替代 HBF `full()` 计数。
5. 检查 32 个 MSHR、每通道开放 DLU 数、页缓冲数量、sector 合并和页面复用顺序。GPU 桥接代码把 `gc.mshrs` 固定为 32；EventLab 路径未使用旧内置 HBF 的 `hbf_cache_entries` 参数。改缓存/合并策略后，应保持请求 ID、返回、failed=0、owned=0 和逐 token kernel 边界完整，并重新验证时序假设。
6. 性能修复同时比较两类量：**模拟 GPU 周期**与**CPU 墙钟**。只优化 Host 回收的 CPU 耗时，不能消除 194 倍模拟周期；只缩短模拟周期，也可能仍受桥接 CPU 开销限制。

## 日志与来源

- 本地诊断：`CPU_ATTRIBUTION_20261103.md`、`full-run.log`、`gpu-diag-run.log`、`kernel6-only-lifecycle-summary.json`、`full-lifecycle-summary.json`。
- node2：`/home/server/hbf-experiments/20261103-gpt2-updated/` 下的 `full-run/`、`smoke-120/`、`gpu-diag-120/`、`control-hbm-120/`、`control-upstream-120/`、`control-hbf-l2-120/`，以及 `source/analyze_lifecycle.py`。各短测退出 124 是设定的 120 秒超时；完整回放退出 124 是设定的四小时超时。
- HBF 后端基线 commit：`8ccb1f353630f893f761b017d621812b7b9d5bf2`。GPU 模拟器 SHA-256 `4effbff65f59e837f6f488f4fcd9a0e8760d08ca2a1cf7d11fcf4885efc4e7ff`；HBF 库 SHA-256 `d2374879974dd422848a4bd0b59be5eb096d71105179eef13ac6d73d4b47a7cf`；配置 SHA-256 `23ec05af952e0f254ad8b5295e5c4c48ae50822b169894b8183bb2f5c4b88ab8`。
- MGPUSim 的 NVIDIA 分支虽能读取同类 NVBit 轨迹，但官方标明尚未验证，且没有接入本项目 HBF；本次未用它生成模型指标。
