# 实验工具与公共基础设施

实验脚本使用 Python 3.10+。生成 workload、调用已构建的 C++ CLI、分析 JSON/CSV 和运行测试均只依赖标准库；这不包含模拟器的 C++ 构建工具。现有实验仍是固定输入子集下的受控诊断，不是任意 trace 的通用分析器。

## 运行与安装

在仓库根目录直接运行脚本，无须先安装 Python 包，例如：

```powershell
python experiments/read_scheduling/run.py --cli build/Release/hbf_eventlab_cli.exe --output out/read-scheduling
```

也支持 `python -m experiments.read_scheduling.run ...`。工作目录、运行命令、具体输入和输出约束见各实验说明。输出目录必须为新目录。

绘图可在独立虚拟环境安装可选依赖：

```powershell
python -m venv .venv
# Windows；Linux 对应 .venv/bin/python
.venv/Scripts/python -m pip install -e ".[plots]"
.venv/Scripts/python experiments/read_scheduling/plot.py out/read-scheduling
```

`pyproject.toml` 的基础运行依赖为空，`plots` extra 声明 matplotlib 和 numpy；安装 Python 包所需的构建后端为 setuptools。源码直接运行不要求 setuptools。保留 unittest，不要求 pytest。依赖版本范围表示支持要求，不是完全锁定的复现实验环境；分享图表时应另外记录所用绘图库版本。

## 公共层职责

| 模块 | 职责 |
|---|---|
| `common/io.py` | UTF-8 JSON/CSV、文件 SHA-256；JSON 不允许 NaN。 |
| `common/provenance.py` | Git 状态及实验自身、公共模块的源码哈希；路径统一为 `/`。 |
| `common/config.py` | 读取输入和实际配置、核对请求是否生效、提供实际几何与容量值。 |
| `common/intervals.py` | 半开区间占用曲线及峰值、积分、均值、忙碌时间；同 Tick 合并，未完成区间保持活动。 |
| `common/model.py` | 当前支持的模型契约：64B 命令、4096B DLU、1ps Tick；不是可调容量预算。 |

各实验依赖公共层；公共层不依赖实验，实验之间不互相导入运行入口。工作负载、实验计划和预期行为仍归各实验所有。

## 参数与分析流程

1. 实验计划或具名实验常量定义一次固定条件，生成 `input.ini` 和 workload。
2. CLI 导出实际生效的 `effective.ini`。
3. 公共层核对每个显式请求参数；输出路径由 CLI 单独设置，`zone_size_dlus=auto` 按实际几何核对派生值。配置未生效、重复键、缺失配置或不支持的 profile 均报错。
4. 分析使用实际几何、MOCS、Pending 和期限；绘图使用实际预算，并检查汇总记录和实际配置是否一致。配置导出允许分段重复 section，但不允许重复键。
5. 策略对比继续检查共享输入和非实验变量的有效配置，避免两组配置同时错误却仅因彼此相同而通过。

固定协议契约集中在 `common/model.py`，对应 C++ `common/types.hpp`；它不是通过解析 C++ 源码取得的，也不是对 C++ 全部默认值的复制。当前有效配置未提供所有固定语义的机器可读元数据，后续新增 profile 时须显式扩展契约，不能静默解释为现有格式。

实验计划可以故意固定 8 page/block；这不同于分析器到处写死“第二个 block 的 DLU 一定为 8”。后者现在从实际几何派生。受控案例中的操作数量、输入形态和预期调度差异仍是有意保留的断言；大型 trace 需要适配分析范围，不能靠删掉正确性断言冒充通用支持。

## 测试与复现边界

默认 `BUILD_TESTING=ON`、`HBF_EVENTLAB_EXPERIMENT_TESTS=ON` 时要求 Python 3.10+。缺失解释器导致配置失败，并提示安装、选择解释器或显式关闭实验测试。`HBF_EVENTLAB_EXPERIMENT_TESTS=OFF` 只验证 C++；`BUILD_TESTING=OFF` 不要求 Python。CI 应保持实验测试开启。

```powershell
python tests/test_experiment_common.py
ctest --test-dir build -C Release -R "^experiment_" --output-on-failure
```

公共层测试覆盖配置漂移、重复键、替代几何和资源预算、区间边界、输出、源码哈希及跨实验依赖。CMake 配置测试分别检查 Python 可用、缺失和显式关闭三种路径。各实验集成测试仍运行真实 CLI、验证状态与确定性，并拒绝损坏的观察记录。

manifest 记录公共工具和实验脚本哈希。重构前的结果保留原 manifest；重新分析或绘图应使用新输出副本并记录新代码，不能把旧结果伪装成用新源码生成的结果。
