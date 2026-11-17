# HBF-EventLab

**An HBF simulator for exploring how host policies, device resources, and scheduling affect request latency and throughput.**

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![arXiv:2611.19861](https://img.shields.io/badge/arXiv-2611.19861-b31b1b.svg)](https://arxiv.org/abs/2611.19861)
[![CI](https://github.com/sjtu-epcc/HBF-EventLab/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/sjtu-epcc/HBF-EventLab/actions/workflows/ci.yml)

**English** | [简体中文](README.zh-CN.md)

HBF-EventLab models High Bandwidth Flash (HBF) as a transaction-level discrete-event system based on the OCP HBF specification. Run read/write workloads, compare policies under finite resource budgets, or connect an external producer through the incremental C++ API. The same Host–Device event loop handles request admission, write assembly, media execution, backpressure, and completion feedback.

[Paper](https://arxiv.org/abs/2611.19861) · [Quick start](#quick-start) · [Experiments](#experiments) · [Documentation](#documentation)

## What can you study?

- **Resource bottlenecks:** vary outstanding-command credits, Pending write contexts, and Bank resources; observe occupancy, assembly timeouts, and completion latency.
- **Addressing and host policies:** compare Global address interleaving, fragment arrival patterns, and FCFS versus fill-open sending.
- **Read/write coordination:** change read-window release and NAND scheduling policies to study read latency and write progress.
- **Fault recovery:** inject program failures and follow Host-driven replay, retries, and logical completion.
- **Application-level effects:** submit requests incrementally from an external execution model. The paper's real-GPU integration combines profiled GPU activity with simulated HBF access delays to estimate TTFT and TPOT.

## Model at a glance

| Component | What it models |
|---|---|
| Host | Global address splitting into 64B commands, sending policies, bounded incremental input, retries, and replay |
| Writes | 4KiB data-layout-unit (DLU) assembly, fragment tracking, ordering, timeouts, erase, and program |
| Reads | 64B access, Pending forwarding, Regular/Batch dispatch windows, ordering groups, and two page-cache slots per Bank |
| Resources | Command credits, Pending contexts, Bank occupancy, UCIe transfers, and finite receive/return buffers |
| Observation | Logical-request and command-attempt latency, Pending lifecycles, Bank activity, JSON/JSONL statistics, and optional wall-time profiling |

The model follows the implemented subset of [OCP HBF Architecture Specification v0.7.0](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf). A [design-decision and specification traceability table](docs/design_decisions.md) connects requirements, model choices, code, and tests. Policy interfaces separate scheduling choices from ordering and resource checks.

## Quick start

The backend requires **C++17 and CMake 3.20+**; the full test suite also requires **Python 3.10+**. The core library has no third-party runtime dependencies. CUDA and model weights are only needed for optional GPU experiments.

```sh
git clone https://github.com/sjtu-epcc/HBF-EventLab.git
cd HBF-EventLab
```

### Linux

Install a C++17 compiler, CMake, Ninja, and Python, then run:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/hbf_eventlab_cli examples/workloads/normal.txt
```

### Windows

Install Visual Studio 2022 with the C++ desktop tools and Windows SDK, CMake, and Python. In PowerShell:

```powershell
.\build.ps1 -Configuration Release
.\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt
```

The normal workload should finish with `STOP Completed workload=Success` and exit code `0`. The Windows script also builds, tests, and runs the examples.

For C++-only testing, configure with `-DHBF_EVENTLAB_EXPERIMENT_TESTS=OFF`. See [Usage](docs/usage.md) for alternative build commands and CLI options, and [Verification](docs/verification.md) for tested environments.

### Configure and inspect a run

On Linux, run:

```sh
./build/hbf_eventlab_cli examples/workloads/normal.txt \
  --config configs/functional.ini \
  --dump-config build/effective.ini \
  --stats-json build/normal-statistics.json

./build/hbf_eventlab_cli examples/workloads/recovery.txt --open 4 --fail-sequence 2
```

On Windows, use `.\build\Release\hbf_eventlab_cli.exe` with the same arguments on one line.

The first command saves the effective configuration and statistics; the second injects a program failure and exercises automatic recovery. Configuration precedence is defaults → INI → CLI overrides. See [Parameters](docs/parameters.md) for units, validation, and available policies.

## Experiments

| Study | Question and entry point |
|---|---|
| Resource limits | How do command credits, Pending capacity, and media service interact? [Capacity experiments](experiments/capacity/README.md) |
| Interleaving and arrival | How does Global interleaving change local DLU assembly? [Timed-arrival experiments](experiments/interleave_arrival/README.md) |
| Host sending | When does prioritizing open DLUs differ from FCFS? [Policy comparison](experiments/arrival_policy/README.md) |
| Read/write scheduling | How do read windows and NAND arbitration affect contention? [Read-scheduling experiments](experiments/read_scheduling/README.md) |
| Real GPU and token timing | How do selected FFN weight accesses affect modeled TTFT/TPOT? [F1–F3 experiment on the GPU branch](https://github.com/sjtu-epcc/HBF-EventLab/tree/feat/real-gpu-sf-experiments/integrations/real_gpu/experiments/gpu_ffn) |

Backend experiment scripts use the Python standard library. Install optional plotting dependencies with `python -m pip install -e ".[plots]"`. Each experiment guide describes its inputs, configuration, checks, and outputs; see the [shared experiment guide](experiments/README.md).

**Paper GPU experiments:** the F1–F3 implementation is on `feat/real-gpu-sf-experiments`, separate from the backend on `main`. It runs Qwen3-0.6B on a real GPU, retaining real weight bytes in the GPU executor while HBF-EventLab models selected weight accesses. Token times combine a frozen GPU activity table with online HBF access latency. See the [GPU setup and experiment guide](https://github.com/sjtu-epcc/HBF-EventLab/blob/feat/real-gpu-sf-experiments/integrations/real_gpu/README.md) for requirements and reproduction.

## Embed the backend

Use the CLI for finite workload files, or link **`HBFEventLab::core`** to submit Global requests from C++:

- [Online session](examples/online_session.cpp): incremental submission, simulation-time boundaries, completion feedback, and input closure.
- [Bounded input](examples/bounded_input.cpp): resident-input budgets and capacity notifications.
- [Tensor bridge](docs/tensor_bridge.md): register tensor ranges and connect access callbacks through the existing Host; link `HBFEventLab::tensor_bridge`.

Public headers are in [include/hbf_eventlab](include/hbf_eventlab/). The [online API contract](docs/usage.md#online-session) describes how callers coordinate input and simulated time.

## Scope

HBF-EventLab is a configurable functional and timing model. It uses symbolic `DataToken` content; NAND service times and buffer sizes are model parameters, and Core Die Channels currently have ideal bandwidth and latency. Results describe the chosen configuration and have not been calibrated to an HBF product.

Read bursts, full AXI/CSR/ADMIN modeling, ECC, and thermal modeling are outside the current backend scope. See [Scope and Limitations](docs/scope_and_limitations.md) for supported responses and input restrictions. Report simulated time separately from simulator wall-clock runtime.

## Documentation

Most detailed technical guides are currently in Chinese.

| Guide | Contents |
|---|---|
| [Usage](docs/usage.md) | Build, workloads, CLI, online API, and exit codes |
| [Parameters](docs/parameters.md) | Unified configuration, defaults, units, and validation |
| [Architecture](docs/architecture.md) | Components, addresses, events, and Host/Device boundaries |
| [Design decisions and OCP traceability](docs/design_decisions.md) | Specification requirements, implementation choices, and verification links |
| [Verification](docs/verification.md) | Test coverage and reproduction commands |
| [Statistics event stream](docs/statistics_event_stream.md) | Buffered JSONL export for long runs |
| [Runtime profiling](docs/runtime_profiling.md) | Optional simulator wall-time diagnostics |

## Citation

If HBF-EventLab contributes to your research, please cite our [paper](https://arxiv.org/abs/2611.19861):

```bibtex
@misc{mei2026hbfeventlab,
  title         = {HBF-EventLab: A Discrete-Event Simulator for Studying Host-Device Coordination in High Bandwidth Flash},
  author        = {Ziqing Mei and Yiwei Huang and Zhiyi Zeng and Yuhao Xiao and Pu Pang and Quan Chen and Deze Zeng and Minyi Guo},
  year          = {2026},
  eprint        = {2611.19861},
  archivePrefix = {arXiv},
  primaryClass  = {cs.AR},
  url           = {https://arxiv.org/abs/2611.19861}
}
```

## Contributing and license

Bug reports, policy experiments, tests, and reviews of specification interpretations are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md). For reproducible reports, include the code revision, workload, effective configuration, and relevant logs.

HBF-EventLab is licensed under [Apache License 2.0](LICENSE).
