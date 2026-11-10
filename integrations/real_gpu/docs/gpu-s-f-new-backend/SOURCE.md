# S/F 复现实验源码

本分支包含 2026-11-05 新后端复现的 S0–S9 与 F1–F3 入口。结果和图表见 [综合报告](README.md)。

## 目录与复用

- [S 系列](../../experiments/gpu_s/README.md)：原生与联合执行器、CPU 数值参照、B2 v2 合同、阶段入口、轨迹采集、逐 token 配对和审计。
- [F 系列](../../experiments/gpu_ffn/README.md)：完整 FFN 选区、在线审计、F1–F3 调度、构建与负例测试。
- `experiments/gpu_ffn/CMakeLists.txt` 直接编译 `../gpu_s/native_s8.cpp`，通过 `GPU_FFN_EXPERIMENT` 选择 F 的 gate/segment 实现。
- `experiments/reproduction_20261105/` 保存批量运行脚本和 S 批次清单示例。脚本保留正式批次的阶段命令与验收检查，将本机固定目录改为参数。
- `third_party/HBF-EventLab` 复用分支已有的 `8ccb1f353630f893f761b017d621812b7b9d5bf2`；模型运行时继续复用 `third_party/llama.cpp-integrated`。

[源码清单](source-manifest.json) 保存发布文件 SHA256，以及与原始 F1 构建记录核对的 13 个文件。F 的 12 个原始文件及共享 `native_s8.cpp` 均与正式记录匹配；README 的构建说明随后作了路径说明修正。S 的 CMake 发布版去掉了无关 A1 目标，其余 S/F 执行与审计源码直接复用本地复现入口。原始模型、二进制和大体积逐事件记录保留在实验归档中。

## 构建

在仓库根目录使用 PowerShell 7、VS 2022 C++ 开发者环境和 CUDA/CUPTI。Python 需安装仓库 `requirements-dev.txt`，并安装 PyTorch 以执行 S 的 CPU 数值参照。原批次为 Python 3.12、PyTorch 2.7.1、CUDA 13.0.88、RTX 5070（SM 120a）。

```powershell
$env:CUDA_PATH = '<本机 CUDA 目录>'
./build.ps1 -Target model -BuildDir ./build-sf-model -CudaRoot $env:CUDA_PATH -CudaArchitectures 120a-real
python tools/integrated_build_identity.py verify --build-dir ./build-sf-model

$env:GPU_S_BASE_BUILD = (Resolve-Path ./build-sf-model).Path
$env:GPU_S_BUILD = Join-Path (Get-Location) 'build-sf-s'
cmake -S experiments/gpu_s -B $env:GPU_S_BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release "-DBASE_BUILD=$env:GPU_S_BASE_BUILD" "-DCUDAToolkit_ROOT=$env:CUDA_PATH"
cmake --build $env:GPU_S_BUILD --parallel 6
./experiments/gpu_ffn/build.ps1 -CudaRoot $env:CUDA_PATH -BaseBuild ./build-sf-model -BuildDir ./build-sf-f

New-Item -ItemType Directory -Force output | Out-Null
python -m unittest discover -s experiments/gpu_ffn -p test_run.py -v
```

使用其他 GPU 时在共享依赖构建步骤修改架构参数。模型依赖构建完成后会封存身份；修改依赖源码后需创建新的依赖构建目录。

## 运行

模型为 Qwen3-0.6B F16 GGUF，报告对应 SHA256 为 `74878aba1d10648eae3d6cfef822f8c59ce3217b3808b35fce2dd02d975952a2`。S0 另需模型转换记录。模型不随源码上传。

S：先将 `experiments/reproduction_20261105/batches.example.json` 复制为新批次目录的 `batches.json`；相对路径以清单所在目录为准。按 [S 入口说明](../../experiments/gpu_s/README.md) 完成 S0–S4，S0 指定 `--numerical-targets experiments/gpu_s/b2_v2.json`，确认各阶段通过。之后运行：

```powershell
./experiments/reproduction_20261105/run-s.ps1 -BatchDir output/gpu-s/<新批次> -BaseBuild ./build-sf-model -BuildDir ./build-sf-s
```

S6 重新采集 3 对预热与 10 对正式样本；新采样不保证仍选择历史 sample-5。S7/S8 复用本次 S6 固定表。旧批次复用时须在清单中显式指定已验收的 S0–S4 目录。

F：将上述模型放到 `models/Qwen3-0.6B-F16.gguf`，创建新的空批次目录，然后运行：

```powershell
New-Item -ItemType Directory output/gpu-ffn/<新批次> | Out-Null
./experiments/reproduction_20261105/run-f.ps1 -BatchDir output/gpu-ffn/<新批次> -BaseBuild ./build-sf-model -BuildDir ./build-sf-f
```

也可使用 [F 的单阶段命令](../../experiments/gpu_ffn/README.md)，通过 `--model` 指定其他模型存放路径，并通过 `--build-dir ./build-sf-f` 指定上述构建。F1 采集一次原生 GPU 轨迹，F2/F3 校验后复用该记录。继续中断批次用 `-Start F2` 或 `-Start F3`，此前阶段必须已通过审计。

## 同步验证

本次源码同步执行 Python 语法与 PowerShell 解析检查、F 的现有单元测试、S 的全部 7 个可执行目标构建、F 的 2 个可执行目标构建及 C++ 在线审计测试。编译验证链接 11 月 5 日封存的模型与后端库，并核对依赖代码与该构建一致。此次同步没有重跑完整 S/F GPU 实验，报告数据仍来自 11 月 5 日批次。
