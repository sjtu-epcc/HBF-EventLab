# HBF EventLab

HBF EventLab is a modular discrete-event simulator for High Bandwidth Flash (HBF) that can be used to study HBF latency and related topics.

**HBF EventLab implements an automated Host–Device feedback loop.** Starting from a finite Global workload, the Host automatically splits and sends requests. The Device processes those requests, schedules resource use, executes operations, and returns completion responses to the Host, driving subsequent request sending and work status updates. The loop also includes wakeups from backpressure caused by finite resources and supports automatic replay recovery initiated by the Host in response to failures. This enables studies of Host–Device coordination during normal reads and writes as well as fault recovery.
**HBF EventLab follows the open OCP specification and provides verifiable evidence for its implementation.** The project takes the [OCP HBF Architecture Specification v0.7.0](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf) as its baseline, focusing on Chapters 4, 5, and 11, and follows the specification's constraints within its implemented scope. Its [Design Decisions and OCP Specification Traceability](docs/design_decisions.md) document links specification clauses, design decisions, code locations, and verification evidence for each item. It clearly distinguishes specification requirements, project interpretations, and model assumptions so that readers can check the basis for the implementation and the boundaries of its applicability.

The project supports research into **Host–HBF coordination, access organization, scheduling, and interactions involving finite resources** within the specification's constraints. Interchangeable policy interfaces for Host request sending, read windows, NAND scheduling, and link arbitration enable policy experiments within the same feedback loop. Request validity, write ordering, error scope, and data ownership are correctness constraints that all replacement policies must maintain.

**The current implementation models a selected subset of the behaviors defined by the specification. It is an uncalibrated model of functionality and timing, and does not represent a complete OCP implementation or imply conformance certification.**

## Current Capabilities

| Area | Implemented Capabilities |
|---|---|
| Host and input | Global byte-address interleaving, splitting requests into 64B commands, filling DLUs across Global entries, and bounded request sending and data retention storage. |
| Write path | 4KiB DLU assembly, duplicate fragment detection, assembly timeouts, acceptance and program ordering, and automatic erase at the start of a write sequence; full 4KiB local write bursts can be explicitly enabled. |
| Read path | 64B reads, Pending forwarding and missing-fragment responses, ordering groups for ordinary reads, Regular/Batch windows, and Bank Cache. |
| Resources and events | Bank execution occupancy, a two-page Cache per Bank, finite receive/return buffers, UCIe transfers, targeted backpressure wakeups, and causal progression within the same Tick. |
| Fault recovery | The Host automatically replays requests based on responses, merges recovery endpoints, drains old attempts, and explicitly terminates affected work when the recovery budget is exhausted or recovery data is insufficient. |
| Addressing and capacity | Component APIs provide Zone swaps, replacement with spare blocks in the same Bank, sparse compensation, and permanent logical retirement; CLI commands are not available for all of these capabilities. |
| Run results | Completion, stalls, run limits, and model errors are distinguished, with separate checks that transient Host and Device activity has drained. |
| Statistics | Optional JSON records request/attempt latency, per-channel Pending occupancy curves, fragment progress, and DLU context lifecycles, including timeouts, retries, and unfinished work. See [Statistics](docs/usage.md#统一统计). |

The media model uses `DataToken` to represent symbolic content. It does not store actual user bytes or NAND cell charge. Core Channel currently uses an ideal model with infinite bandwidth and zero latency; the fixed NAND latencies and buffer parameters cannot be used directly to predict real product performance.

Read bursts, full AXI/CSR/ADMIN, ECC, a temperature subsystem, and a complete GPU–HBM–HBF path are currently unsupported. Inputs such as explicitly ordered reads accessing Pending DLUs and additional completion dependencies for WRITE requests also have specific restrictions. See [Scope and Limitations](docs/scope_and_limitations.md) for details.

## Quick Start

The project uses C++17 and CMake 3.20 or later. The core library has no third-party runtime dependencies. Run all commands below from the repository root. Verified platforms include:

- **Linux**: x86_64 / Ubuntu 26.04 LTS / GCC 15.2.0.
- **Windows**: x64 / Visual Studio 2022.

See [Verification](docs/verification.md#evidence) for complete toolchain details and regression results.

### Linux (Ubuntu / Debian)

1. **Install the build tools.** You need a compiler that supports C++17, CMake, and Ninja. On Ubuntu / Debian, install them with the following commands, then use `cmake --version` to confirm that the version is at least 3.20:

   ```sh
   sudo apt-get update
   sudo apt-get install -y build-essential cmake ninja-build
   ```

2. **Configure, build, and test.** The following commands build Release and run the full CTest suite:

   ```sh
   cmake -S . -B build-linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
   cmake --build build-linux-release --parallel "$(nproc)"
   ctest --test-dir build-linux-release --output-on-failure --parallel "$(nproc)"
   ```

   Ninja is a single-configuration generator: select Debug or Release with `CMAKE_BUILD_TYPE` during configuration. Executables are placed directly in the build directory. Use separate directories for Debug and Release, and a new directory when switching generators. See the [Contributing Guide](CONTRIBUTING.md#testing-and-verification) for full Debug/Release regression commands.

3. **Run the workloads.** Run the normal read/write workload, followed by the automatic recovery workload that injects a single program failure on page 2:

   ```sh
   ./build-linux-release/hbf_eventlab_cli examples/workloads/normal.txt
   ./build-linux-release/hbf_eventlab_cli examples/workloads/recovery.txt --open 4 --fail-sequence 2
   ```

4. **Run the component examples.** Run the low-level component and standalone Device examples directly:

   ```sh
   ./build-linux-release/hbf_eventlab_demo
   ./build-linux-release/hbf_eventlab_device_demo
   ```

### Windows

1. **Install the build tools.** Install the Visual Studio 2022 Desktop development with C++ tools, the Windows SDK, and CMake 3.20 or later.

2. **Build and test.** Run the following command in PowerShell:

   ```powershell
   .\build.ps1 -Configuration Release
   ```

   The script builds the project, runs CTest, and runs all three example entry points. Use a new build directory when switching generators. See [Usage](docs/usage.md) for manual CMake commands.

3. **Run the workloads.** Run the normal read/write workload, followed by the automatic recovery workload that injects a single program failure on page 2:

   ```powershell
   .\build\Release\hbf_eventlab_cli.exe examples\workloads\normal.txt
   .\build\Release\hbf_eventlab_cli.exe examples\workloads\recovery.txt --open 4 --fail-sequence 2
   ```

On both platforms, the normal read/write and single-failure recovery workloads are expected to print `STOP Completed workload=Success` and return exit code `0`. The recovery example includes failed command attempts, but the corresponding logical work succeeds after replay. See [Usage](docs/usage.md) for the complete CLI parameters, fault scenarios, and exit codes.

## Input and Code Entry Points

The first line of a workload must be `HBF_EVENTLAB_WORKLOAD 1`, and request addresses must use **Global byte addresses**. File order does not imply that the previous request has completed. See the [Input Format](docs/usage.md#有限-workload-格式-v1) for the meanings of dependencies, content identity, and group ordering. Start with the [Normal Read/Write Example](examples/workloads/normal.txt) and the [Recovery Example](examples/workloads/recovery.txt).

- `hbf_eventlab_cli`: the main entry point for Global workloads and the Host–Device feedback loop.
- `hbf_eventlab_device_demo`: a standalone example driven by Device transactions.
- `hbf_eventlab_demo`: a functional example that explicitly operates low-level components.

The C++ namespace is `hbf_eventlab`, public headers are in [include/hbf_eventlab/](include/hbf_eventlab/), and the core CMake link target is `HBFEventLab::core`. See [Architecture](docs/architecture.md) for the division of responsibilities among interface declarations, policy implementations, and runtime state.

## Documentation

| Document | Contents |
|---|---|
| [Design Decisions and OCP Specification Traceability](docs/design_decisions.md) | Links specification clauses, design decisions, code locations, and verification evidence for each item to make the implementation basis traceable and verifiable, and documents project interpretations and model assumptions. |
| [Architecture](docs/architecture.md) | Component responsibilities, address models, event lifecycles, and Host/Device boundaries. |
| [Parameters](docs/parameters.md) | Unified configuration, defaults, units, validation, and reproducible configuration snapshots. |
| [Usage](docs/usage.md) | Building, workload format, CLI, configuration, exit codes, and examples. |
| [Verification](docs/verification.md) | Test matrix, verified environments, results, and reproduction commands. |
| [Scope and Limitations](docs/scope_and_limitations.md) | Supported response subset, input restrictions, unimplemented features, and performance model boundaries. |
| [Contributing Guide](CONTRIBUTING.md) | Issue reporting, development workflow, testing, and documentation synchronization requirements. |

## Contributing and Reproducing Experiments

Issue reports, additional tests, reviews of specification interpretations, and proposals for policy experiments are welcome. See the [Contributing Guide](CONTRIBUTING.md) before changing behavior or interfaces. The design decisions and specification traceability tables are key references for understanding the model's semantics.

When using this project for experiments, record the code version or commit, workload, complete configuration, fault inputs, and random seeds. Explain the meaning of each metric and the basis for the parameters. Report simulated Ticks, program wall-clock execution time, and real hardware latency separately.

## License Status

This project is licensed under the **Apache License 2.0 (Apache-2.0)**. See [LICENSE](LICENSE) for the complete license terms.
