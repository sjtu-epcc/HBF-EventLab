# Release status

Release: RealGPUAcess_v0.
Revision: incremental-host-72e0745.
Upstream backend: 72e074508c462dceb7d42962c4885ecf17cc4d0d, six recorded local patch files.

Release verification passed. See
[release-validation.json](docs/validation/release-validation.json) and
[validation scope](docs/VALIDATION.md) for the acceptance records.

Fresh release-directory builds passed 716 core CPU/CUDA/injection tests,
18 independent operator tests and five Python validator tests. Two representative
model cases ran in all three modes (six processes); complete logits and tokens
match corresponding accepted candidate and prior main-backend outputs. The
accepted nine-case, 27-process candidate matrix is retained as inherited evidence.

The source archive preserves required nested build-info files. Model weights,
local profiles, toolchains, build outputs and large traces are not distributed.

The source uses functional filenames, relative build dependencies, English
release documentation and relative README examples. The architecture image
includes the actual SimulationSession/HostDriver/DeviceDriver path.
