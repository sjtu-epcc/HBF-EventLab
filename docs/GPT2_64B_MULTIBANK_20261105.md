# GPT-2 GPU/HBF 接入修正与验证（2026-11-05）

本轮在 `aa814002a7b6dd2a530d8717375daa9877b0eb04` 的实验快照上修改 GPU 接入层；EventLab 后端仍为 `8ccb1f353630f893f761b017d621812b7b9d5bf2`。真实预训练 GPT-2 124M、WikiText-2、8 输入 token、最多 4 输出 token、FP32、batch 1、KV cache 和 1480-kernel 轨迹保持不变。真实 GPU 不提供本轮时间指标。

## 修改

- GPU 读请求按其覆盖的 64B 对齐单元提交给增量 HostDriver。同一单元的兼容在途 sector 合并；GPU 原请求须等待自己覆盖的全部 64B 单元完成。写入继续按完整 4KiB 页聚合。没有修改 HostDriver 的增量输入、容量通知和最终完成反馈。
- GPU 桥接可读取后端 INI 格式的 `device.geometry.*` 参数，并打印实际生效几何结构。本次临时实验配置：16 channel × 4 die/channel × 16 bank/die × 8 page/block × 16 block/bank，另有每 bank 2 个备用 block。每 channel 仍为 8192 个逻辑页；每 bank 两个 4KiB 缓存槽由后端固定实现。该几何来自后端示例的 die/bank/page 结构，**不是已确定的产品规格**。旧 GPU 配置的 `512 subarrays` 不传入 EventLab。
- 多页 block 的权重预加载让同一逻辑 block 的页面共用内容版本。初版在第 1040 页因每页使用不同版本而留下 16 条 Host 请求；修正后 121551 个权重页全部通过普通 Host 写路径完成。

## 已完成验证

- node2 独立目录：`/home/server/hbf-experiments/20261105-gpt2-64b-multibank`。旧实验目录未改动。EventLab 加 GPU 接入测试 **524/524 通过**；模拟器编译退出码 0。
- 8 页/block、64B 读、多 bank 的 120 秒短测按设定超时退出 **124**，此前完成 5 个 kernel；权重预加载完成，前 5 个 kernel 的 GPU 请求 `failed=0`、`owned=0`、全部返回，Host/Device 排空。
- 较长隔离回放留下 15 个完整 kernel 的日志，但外层 SSH 会话断开，缺少可靠的整次退出码，因此不能将它作为完整模型结果。第 6 个 kernel 的结束记录包含 `admitted=completed=returned=228864`（累计）、`failed=owned=0`、`drained=1`。

| 第 6 个 kernel 指标 | 2026-11-03 旧接入 | 本轮 64B + 多 bank |
|---|---:|---:|
| 模拟 GPU 周期 | 30,999,754 | 4,204,937 |
| GPU 32B sector 请求 | 221,184 | 221,184 |
| Host 读命令/请求片段 | 1,332,928（20,827 个整页读 × 64） | 159,368（64B 读） |
| NAND sense | 18,306 | 1,728 |

周期约减少 **7.37 倍**。两项接入变化同时发生，不能单凭本次对照分离 64B 粒度与 bank 并行度各自的贡献；如需归因，应分别运行单因素对照。第 6 个 kernel 仍比普通 HBM 对照的 160036 周期慢约 26 倍。上述数字只适用于第 6 个 kernel，**不是 TTFT/TPOT**。

## 完整回放

已在独立目录启动有 24 小时上限、`nice -n 10`、`ionice -c 2 -n 7` 的完整回放。输出在 `full-run/`，启动器写 `full-launcher.rc`，模拟器写 `full-run/run.rc`。只有退出码 0、1480 个 kernel 全部完成、所有请求返回、`failed=owned=0`、Host/Device 排空、逐 token 边界完整后，`source/collect_metrics.py` 才生成模拟 TTFT/TPOT。当前尚无可发布的完整指标。

## 来源核对

- node2 本轮桥接源码 SHA-256 `48eb1355316825accaa2bcf3d1e2785bf16194b989f8875401990c914d1b7de5`（传输时换行格式与 Windows 工作副本不同）。
- GPU 页控制器 SHA-256 `b89f93ad539e302f531c80cea156552972e5b2f0ce38b13f94450eb2ccdb0c`。
- 几何配置 SHA-256 `8f461403303f8583738c1479b9f4e6cb6b421fe7ab9b468a92c22ab07ee537f5`。
- 模拟器可执行文件 SHA-256 `3f2854a067fbdaf19db0da567aea492e3245c031885e0eaa101658bf6c756818`。
