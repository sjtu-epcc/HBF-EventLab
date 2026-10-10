# Contributing to HBF EventLab

Issue reports, specification interpretations, tests, documentation improvements, and policy implementations are welcome. See [Scope and Limitations](docs/scope_and_limitations.md) for the project's capability boundaries, [Architecture](docs/architecture.md) for component responsibilities, and [Design Decisions and OCP Specification Traceability](docs/design_decisions.md) for specification references and implementation choices.

## Reporting Issues

Please provide the smallest example that can reproduce the issue independently, along with:

- The code version or commit, operating system, compiler, CMake version, and Debug/Release configuration.
- The workload, configuration, and complete run command; include the injection sequence and random seed when faults or randomized policies are involved.
- Expected results, actual results, exit codes, and relevant log excerpts. Distinguish failed command attempts, failed logical work, and the Runner's stop status.
- If you believe the behavior violates the specification, include the specification version, section or table number, applicable conditions, and your interpretation.

For API issues, a minimal C++ example can replace the workload. Attach large logs separately, and avoid including an entire build directory or dependencies on local paths in the reproduction steps. Assess unsupported input combinations against the model's capability boundaries first; do not automatically classify them as a particular OCP error code.

## Development Workflow

1. For substantial interface changes, changes to specification interpretations, or additions to the model's scope, first describe the problem, applicable boundaries, and verification method in an Issue or PR. Small fixes and documentation corrections can be submitted directly as PRs.
2. Create a working branch from the target branch. Keep each PR focused on one clear problem, and avoid unrelated refactoring or repository-wide formatting.
3. Modify the code and add tests and documentation appropriate to the scope of the change. Follow existing component and interface boundaries, and avoid maintaining copies of the same state in different modules.
4. Complete the necessary verification before submitting the PR. Describe the behavioral change, its basis, test results, and compatibility impact. Explicitly identify configurations you could not run.

## Model and Interface Principles

- **Policies must preserve correctness constraints.** Replacing a scheduling policy must not bypass request legality, write ordering, error scope, mapping ownership, or similar rules. New policies may select only legal candidates; the Device remains responsible for checking current execution conditions.
- **State is maintained by its owning component.** Queries and probes without side effects must not reserve resources or change lifecycles prematurely. Issuing an operation must recheck the necessary conditions, and completion must be managed through an explicit operation identity.
- **Distinguish completion boundaries.** Host handoff, formal Device acceptance, media issue, media completion, and response delivery have different meanings. Changes must preserve paired acquisition and release of credits, buffers, content dependencies, and leases.
- **The Host acts on public feedback.** The production Host does not read the Device's private recovery state or NAND state to fill in recovery data. Tests may inspect internal state, but such inspection must not replace verification of the feedback loop's behavior.
- **Preserve event causality and reproducibility.** Follow the existing same-Tick rounds, deadline batches, and notification rules. Do not mask progress problems by arbitrarily adding Ticks or using unbounded polling. Provide explicit seeds when introducing randomized policies.

The concrete contracts for these principles are defined in the architecture and design decision documents. If a contract must change, propose the change together with the affected components, tests, and documentation, rather than modifying only a local code path.

## Testing and Verification

Run all commands below from the repository root. See the [README Quick Start](README-EN.md#quick-start) for dependencies and installation steps, and [Verification](docs/verification.md) for the test matrix, environments, and interpretation of results.

### Linux

1. **Build and run the full regression suite.** Use Ninja to configure, build, and verify Debug and Release separately:

   ```sh
   cmake -S . -B build-linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
   cmake --build build-linux-debug --parallel "$(nproc)"
   ctest --test-dir build-linux-debug --output-on-failure --parallel "$(nproc)"

   cmake -S . -B build-linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
   cmake --build build-linux-release --parallel "$(nproc)"
   ctest --test-dir build-linux-release --output-on-failure --parallel "$(nproc)"
   ```

   Ninja's configuration is determined during generation; `--config` and `-C` do not switch it. Both build directories are ignored by `.gitignore`. Detailed CTest logs are stored in `Testing/Temporary/LastTest.log` within each build directory.

2. **Run the examples.** Also run the component examples and the Host feedback loop (the commands below use Release; substitute the build directory for Debug):

   ```sh
   ./build-linux-release/hbf_eventlab_demo
   ./build-linux-release/hbf_eventlab_device_demo
   ./build-linux-release/hbf_eventlab_cli examples/workloads/normal.txt
   ./build-linux-release/hbf_eventlab_cli examples/workloads/recovery.txt --open 4 --fail-sequence 2
   ```

   All commands above are expected to return exit code 0. Both workloads should print `STOP Completed workload=Success` and `host_drained=1 device_drained=1`. Debug and Release on Linux x86_64 / Ubuntu 26.04 LTS / GCC 15.2.0 have passed the full regression suite and these examples.

3. **Run targeted scenarios.** During development, first verify cases relevant to your change, such as a single Host recovery:

   ```sh
   ctest --test-dir build-linux-debug -R '(^|_)recovery_once$' --output-on-failure
   ```

### Windows

1. **Build and run the full regression suite.** Run the Debug and Release build scripts in PowerShell:

   ```powershell
   .\build.ps1 -Configuration Debug
   .\build.ps1 -Configuration Release
   ```

   The script includes CTest and example runs. See [Usage](docs/usage.md) for dependencies and manual CMake commands.

2. **Run targeted scenarios.** During development, first verify cases relevant to your change, such as a single Host recovery:

   ```powershell
   ctest --test-dir build -C Debug -R '(^|_)recovery_once$' --output-on-failure
   ```

### Requirements for Writing Verification Tests

The scope of verification should match the change:

| Change Type | Verification Requirements |
|---|---|
| Behavior fixes | Where possible, add a test that reproduces the issue before the fix, checking observable results and necessary invariants. |
| New policies or parameters | Verify that the policy does not bypass legality, ordering, ownership, or progress constraints; record defaults and experiment configurations. |
| Public interfaces, state lifecycles, or build changes | Rebuild all affected targets, and run the full Debug/Release regression suite and relevant examples before merging. |
| Other code changes affecting runtime behavior | Run targeted cases first, then complete the full regression suite before merging; historical test results alone are insufficient. |
| Documentation-only changes | Check terminology, code references, relative links, commands, and capability descriptions; rebuilding is not always required. Actually verify new commands or input examples. |

Tests should cover meaningful behavioral boundaries rather than merely repeat implementation steps.

## Specification Traceability and Documentation Synchronization

When changing observable behavior, specification interpretations, default policies, or model parameters, check whether the [Design Decisions](docs/design_decisions.md) document needs updating:

- Specification requirements: provide the version, section or table number, applicable conditions, and implementation and test locations.
- Specification ambiguities: state the competing interpretations, the current choice, and its implications; do not present the project's interpretation as specification text.
- Internal mechanisms or model assumptions: explain why they are needed, which invariants they preserve, parameter sources, and known limitations; explicitly identify parameters without a calibration basis.
- Replaceable policies: describe the range of legal candidates, default behavior, and configuration method; the existence of an interface does not prove that arbitrary implementations conform to the specification.

Use the existing classification of specification relationship × entry type. Preserve existing entry IDs rather than renumbering them for formatting. Ordinary internal refactoring or spelling corrections do not require new decision entries; update affected links if code locations change.

Update documentation according to its responsibilities, and avoid duplicating large sections of the same rules:

| Change | Corresponding Document |
|---|---|
| Component responsibilities, data flow, or lifecycles | [architecture.md](docs/architecture.md) |
| Input format, CLI, default configuration, or exit codes | [usage.md](docs/usage.md) |
| Specification interpretations, mechanisms, or parameter rationale | [design_decisions.md](docs/design_decisions.md) |
| Verification scope, environments, or test evidence | [verification.md](docs/verification.md) |
| Added or removed capability limitations | [scope_and_limitations.md](docs/scope_and_limitations.md) |
| Project positioning, quick start, or documentation entry points | [README.md](README.md) |

## Code and Submission Contents

The project uses C++17. Place public headers in `include/hbf_eventlab/` and implementations in `src/`. The C++ namespace is `hbf_eventlab`, and the core CMake link target is `HBFEventLab::core`. Follow the naming and organization of nearby code; headers should clearly express responsibilities and ownership.

Use UTF-8 for text and follow the line-ending rules in [.gitattributes](.gitattributes). Do not commit build artifacts, IDE caches, local absolute paths, or temporary logs. Prefer repository-relative paths and small inputs for reproduction materials.

A PR description should enable a reviewer who has not participated in the discussion to understand the problem, triggering conditions, behavior after the change, relevant specification or decision entries, verification performed, input or interface compatibility, and known limitations. If changes to simulation semantics alter experiment results, explicitly explain why.

When submitting code, documentation, or data, ensure you have the right to provide it, and identify reused sources and notices that must be retained. See the [README](README-EN.md#license-status) for the repository's license status.
