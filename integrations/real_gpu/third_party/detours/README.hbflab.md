# Microsoft Detours dependency

Source: https://github.com/microsoft/Detours

Version: v4.0.1, commit `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`.

Only the core `src` library files and MIT license are vendored, unchanged. Samples,
tools and prebuilt binaries are not included. `uimports.cpp` is included by
`creatwth.cpp`, not compiled separately. The disassembler wrappers are the upstream
core-library build inputs. `upstream-Makefile.txt` records that file list.

HBFLab uses Detours only in the optional Windows x64 process launcher/injection DLL.
No system CUDA DLL is replaced and no target executable is modified on disk.
