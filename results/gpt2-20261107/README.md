# GPT-2 124M + WikiText-2 + 模拟 GPU/EventLab HBF 结果

完成时间：2026-11-07（Asia/Shanghai）。正式回放目录：`/home/server/hbf-experiments/20261107-gpt2-retired-history/full-run/`。本目录保存可公开复核的结果摘要、捕获与原生参照元数据、运行配置和退出码。

## 指标

| 指标 | 模拟结果 |
|---|---:|
| TTFT（首 token 完成时间） | **72.992536 ms** |
| TPOT（末 token 与首 token 完成时间差 ÷ 3） | **48.000169 ms/token** |
| 四个 token 全部完成 | 216.993043 ms |
| 模拟 GPU 总周期 | 305,960,191 |

输入为 WikiText-2 test parquet 第 3 行的 8 个 token，提示文本 ` Robert Boulter is an English film`；预训练 GPT-2 124M、FP32、batch 1、KV cache 自回归解码，输出 4 个 token，生成文本 ` critic and writer.`。捕获、native reference 与模拟回放的 token ID 一致。核心时钟 1410 MHz。权重通过普通 Host 写路径初始化，共 121,551 个 4KiB 页，`seeded_pages=0`。设备几何采用临时实验配置：16 channel × 每 channel 4 die × 每 die 16 bank；每 bank 两个 4KiB 页缓存槽。GPU 接入层为 512 个 64B MSHR；死锁检测保持开启。

| 输出序号 | 阶段 | token ID | kernel 数 | 完成周期 | 完成时间 (ms) | 本 token 间隔 (ms) |
|---:|---|---:|---:|---:|---:|---:|
| 0 | prefill | 4014 | 343 | 102,919,476 | 72.992536 | 72.992536 |
| 1 | decode | 290 | 379 | 170,216,747 | 120.721097 | 47.728561 |
| 2 | decode | 6260 | 379 | 238,225,156 | 168.954011 | 48.232914 |
| 3 | decode | 13 | 379 | 305,960,191 | 216.993043 | 48.039032 |

## 验收

- 模拟器和外层脚本退出码均为 **0**，1480/1480 个 kernel 完成，四个 token 的 kernel 边界按顺序恰好覆盖全部回放。
- 70,710,912 个 GPU 请求全部 admitted/completed/returned；逐请求 CSV 审计 `valid=true`，`failed=0`、`owned=0`，Host/Device 最终排空（`drained=1`）。
- `source/collect_metrics.py` 严格核对捕获和 native reference 的输入/输出 token、初始权重写入、逐 kernel 周期、请求生命周期及 token 边界后才生成 `summary.json`。
- 完整模拟耗时 59,857.1 秒墙钟（约 16 小时 38 分），峰值 RSS 9,404,804 KiB。墙钟仅是模拟计算成本，**不参与 TTFT/TPOT**。
- 原始结果包内 15 个文件的 SHA-256 均在本地核对通过。本目录收录其中 12 个小文件，并用 `SHA256SUMS` 校验；完整 70,710,912 请求的生命周期 CSV（约 11 GB）保留在 node2 的 `full-run/eventlab.csv`。完整 `run.log`（70,683,668 字节）保留在 node2 和本地结果包，SHA-256 为 `7ce37ce68270364dbbc33c5e1f2b4ad10dd2899da50757cf1032ab055d15018c`。日志中的严格审计结论也保存在 `summary.json`。

## 来源与适用范围

源码提交 `56d4490c2a53317ed7245f36ff309f308d1bc487`。模拟器二进制 SHA-256：`f3a6724920c190e66adc1d697efba69c4ace764f898229f183db94c9ca250ff1`；GPT-2 权重 SHA-256：`248dfc3911869ec493c76e65bf2fcf7f615828b0254c12b473182f0f81d3a707`；WikiText-2 数据 SHA-256：`5f1bea067869d04849c0f975a2b29c4ff47d867f484f5010ea5e861eab246d91`。源码、配置和二进制哈希见 `full-source.sha256`；本目录文件哈希见 `SHA256SUMS`。权重、数据集、完整 NVBit 轨迹、请求 CSV 和模拟器二进制未收入仓库。

这是基于 NVBit 捕获轨迹、Accel-Sim GPU 核心和增量 EventLab HBF 后端得到的**模拟 GPU 时间**。KV cache 与激活在模拟 HBM，HBF 用于模型权重路径；指标不含分词、模型加载、Host launch 和排队时间。设备几何和时序尚未按实物校准，因此这些数值用于该配置下的架构实验，不代表真实硬件实测延迟。

结果文件：`summary.json`、`per-token.csv`、`capture-result.json`、`native-reference-result.json`、`replay-manifest.json`、`run.rc`、`full-launcher.rc`、`run.time`、`gpgpusim.config`、`gpt2-eventlab-device.ini`、`full-source.sha256`、`SHA256SUMS`。
