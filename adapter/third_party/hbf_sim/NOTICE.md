# Imported HBF-Sim helpers

Source: https://github.com/jager197/accel-sim-hbf
Commit: 3415a3171d9312fac6a835bc1aa188f7aab685c0 (dev)
Paper: https://arxiv.org/abs/2609.29246

`hbf_mapping.h` is retained unchanged. `write_coverage.hpp` retains the `hbf_write_coverage_t` body from `hbf/hbf_controller.h`, adding C++ includes, an isolated namespace and the 128-bit GPU byte-mask type to decouple it from GPGPU-Sim headers. The upstream LICENSE is retained verbatim. These helpers do not by themselves import the full upstream controller or GPU core simulator. The complete upstream artifact is installed separately for reference validation.

