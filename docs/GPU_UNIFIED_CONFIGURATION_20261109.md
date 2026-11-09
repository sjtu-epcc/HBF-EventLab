# GPU 桥接统一配置入口：修改与小规模验证

日期：2026-11-09。基于实验分支 `bec042748246f02cce2d0f848505e8854ffc8bc8`，主仓实验分支 `exp/simulated-gpu-unified-config`。原完整实验结果目录不变。

## 改动

- `configured_backend.hpp` 是 Accel-Sim 桥接和本地 smoke 共用的装配实现：完整 `read_configuration` → `resolve_configuration` → description/execution/Host 构造。原后端 `EventLab/` 未修改。
- INI 的 device（含 capability/selected/topology/capacity_policy）、host、execution、faults、run 均传入执行；事件数量上限与时间上限也作用于初始化。时间上限为绝对后端时间，包含预加载。
- `hbf_eventlab_bridge.cc` 不再自己构造 MOCS/Pending、介质时延或 Host 配置。必须给出 `HBF_EVENTLAB_DEVICE_CONFIG`。
- `config/gpt2-eventlab-device.ini` 明确写入旧实验实际使用的数值：16×4×16、256 MOCS、32 Pending、sense/program/erase=15/200/2000 μs、grade 3、4096 活动片段、burst4096。未填写的字段由统一解析器补默认值，并导出完整 effective INI。
- 配置写出到 `output.effective_config`；留空则写在输入文件旁，文件名追加 `.effective.ini`。禁止输出覆盖输入。
- 旧的 `HBF_EVENTLAB_PAGES_PER_CHANNEL/UCIE_GRADE/BURST_WRITES/RESIDENT_FRAGMENTS/FAIL_ONCE` 环境变量明确拒绝，要求迁入 INI。
- GPU 的 tR/tPROG/tBERS 不再决定后端时延。GPU channel 数和 channel bandwidth 仅作连接一致性检查，必须与 INI 匹配。GPU 核心时钟、路由、MSHR、上游合并窗口、写聚合等仍属 GPU 适配层。

## 当前支持边界

页级稀疏驻留映射保持原语义：要求 `address.global_base=0`、`interleave_bytes=4096`，其他值立即拒绝。这里没有宣称支持任意地址交织。

CLI 的 output.trace、statistics_json、statistics_events_jsonl、runtime_profile_json 输出消费者尚未安装到本适配器，非空值明确拒绝，避免配置被忽略。原 GPU 生命周期 CSV 与 HBF_DIAG_OUTPUT 诊断保留。这轮重点是统一后端模型参数，不是改造统计文件导出。

## 本地小实验

Windows/MSVC Release，直接编译原 EventLab 后端、组员 GpuPageController 和上述共享装配组件；无需 CUDA。使用正式实验的完整后端几何配置：

1. 经增量 Host 正常写入 16 个 4KiB 页；
2. 每页提交两个相邻 32B sector；
3. 合并为 16 个 64B 后端读；
4. 检查内容身份、48 次请求完成、Host/Device/controller 排空。

结果：`completed=48, reads64=16, merges=16, programs=16, senses=16, accepted=32, drained=1`。读取阶段 **15.001841 μs**。这是确定性适配器小负载结果，不是 GPT-2 指标。

另用紧凑几何 2 channel × 1 die × 2 bank × 2 page/block 验证：

| 配置变化 | 观察结果 |
|---|---|
| sense 0.1 → 1 μs | 读取阶段 0.402841 → 4.002841 μs，sense 次数不变 |
| burst4096 → fragment64 | 接受命令 32 → 1040，program 均为 16 |
| 一次 program fail | 故障配置进入执行路径，恢复后正常完成 |
| effective INI 写出再读入 | 逐字段规范化文本相同 |
| 非法 MOCS、Selected > Supported、未知字段、不支持的地址/输出 | 明确拒绝 |
| 同 Tick 开放端点、事件/时间预算 | 保持输入边界，限制可触发 |

详细输出见 `results/gpu-unified-config-smoke-20261109/smoke.txt` 和 `effective.ini`。

## 回归结果与限制

12 个新增配置测试、13 个已有 GPU 适配测试均通过。全量 CTest 初次 536/537；读调度集成测试在本机深层输出路径出现两例 CLI 子运行失败。将相同测试的输出目录移至较短路径后完整通过：14 个场景、确定性重复、公平性和损坏记录检查。未为此修改原后端或断言；该失败尚未独立定位到具体系统调用，不能仅据短路径重跑就断言原因。

本轮未运行 Accel-Sim/GPT-2：本机未安装 WSL 发行版，且没有服务器上的完整轨迹、模型与 Linux 模拟器二进制。共享装配与 sector 路径已编译运行；`source/hbf_eventlab_bridge.cc` 仍需在组员的 Linux GPU 模拟器工程中编译、链接和短轨迹验证，不能将当前结果称为完整 GPU 前端集成验收。

## 复现与下一轮

```sh
cmake -S source -B build-unified -DCMAKE_BUILD_TYPE=Release
cmake --build build-unified -j
ctest --test-dir build-unified --output-on-failure
./build-unified/gpu_configuration_tests experiment config/gpt2-eventlab-device.ini effective.ini
```

Windows 多配置生成器使用 `--config Release`、`ctest -C Release`，程序在 `build-unified/Release/`。

Linux Accel-Sim 重新构建时，替换桥接源码，更新 `gpu-headers/`，链接本轮重建的后端加适配层库；原第三方 GPU 编译环境按组员已有方法准备。不要误用旧二进制。

新增 `source/run_unified_replay.sh` 接收二进制、GPU config、kernelslist、image-pages、全新输出目录和可选 backend INI。它清除旧后端环境覆盖、保存输入配置、默认 120 秒超时。GPU bandwidth 默认188，与基线 grade3一致；修改档位时也需修改该连接参数。

```sh
bash source/run_unified_replay.sh /path/accel-sim.out /path/gpgpusim.config \
  /path/short/kernelslist.g /path/image-pages.txt /path/new-smoke-run
```

先用小轨迹通过 exit=0、完成返回与排空检查，再单独授权完整1480-kernel运行。超时124仅表示运行限制，不是成功。旧 run_replay* 脚本留作历史复现，不用于新配置入口。
