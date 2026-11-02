# Dependencies

| Component | Version / identity | Distribution |
|---|---|---|
| HBF-EventLab | Repository root; publication baseline 8ccb1f353630f893f761b017d621812b7b9d5bf2, actual source digest verified at build/run | Shared source in ../../..; no vendored copy |
| llama.cpp | b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9 plus existing private overlay | Complete source in ../third_party/llama.cpp-integrated |
| Detours | Existing accepted frontend dependency | Source and license in ../third_party/detours |
| CUDA | Toolkit 13.0, CUPTI, cuBLAS and runtime DLLs | External installation |
| Compiler | Visual Studio 2022 C++ Build Tools | External installation |
| Build tools | CMake >= 3.24 and Ninja | Located by ../build.ps1 |
| Python | 3.12 validated; runtime packages in ../requirements.txt | External environment |

The historical upstream backend provenance and original/patched hashes are in
[the origin record](../third_party/hbf-eventlab-origin.json); the original changes
are in [the overlay patch](../third_party/hbf-eventlab-overlay.patch). These files
are retained for attribution, not applied to the repository backend. The guarded
fault interface and Host optimizations are already part of the backend baseline.
The current build seal verifies the actual repository source inventory.

The framework overlay metadata had two stale hashes in the imported snapshot.
They were reconciled against the unchanged source bytes, the upstream release
manifest and the import inventory; no framework implementation was altered.
The [original metadata](validation/upstream-llama-overlay.json) is retained, and
the current overlay includes its reconciliation evidence.

requirements-dev.txt adds matplotlib and Pillow for architecture rendering.
GGUF parsing uses the bundled framework gguf-py package. No installed runtime
source path depends on the development workspace. Model GGUF files, conversion
records and generated local profiles are caller-supplied inputs.

Third-party source, notices and license files retain their original content and
languages. Release-authored documentation is English.
