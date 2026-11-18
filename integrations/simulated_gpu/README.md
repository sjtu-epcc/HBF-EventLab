# Simulated GPU integration

The adapter translates GPU sector requests into work for the incremental HBF Host. The same Host, Device, and event queue serve both the C++ tests and the Accel-Sim bridge. The public CMake target is `HBFEventLab::simulated_gpu`.

From the repository root, build the controller and run its tests:

```sh
cmake -S . -B build -DHBF_EVENTLAB_SIMULATED_GPU=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release -R '^gpu_' --output-on-failure
```

The tests run on a CPU. The `gpu_configuration_experiment` case preloads 16 pages through the Host and issues pairs of adjacent 32-byte reads. Its reference result is 48 completed requests, 16 merged reads, 16 senses, 15,001,841 ps for the read phase, and all components drained.

### Build the Linux Accel-Sim executable

Prepare a dedicated checkout of [HBF-Sim at 3415a31](https://github.com/jager197/accel-sim-hbf/tree/3415a3171d9312fac6a835bc1aa188f7aab685c0) and run its `make bootstrap` with its documented Linux/CUDA prerequisites. This installs its HBF changes into GPGPU-Sim at `6c3cf4f`. From HBF-EventLab, run:

```sh
bash integrations/simulated_gpu/accel_sim/build.sh "$SIM_ROOT" "$PWD/build-sim-gpu"
```

The script applies the EventLab hooks to that external checkout, installs the bridge sources into it, builds the two EventLab static libraries with position-independent code and links them into the simulator. Use paths without spaces for these two checkouts and the Make build. The trace-driven executable is `$SIM_ROOT/gpu-simulator/bin/release/accel-sim.out`.

The bridge implementation and controller come from the [unified configuration snapshot](https://github.com/sjtu-epcc/HBF-EventLab/tree/d32959113553ed9b38eb02bb10f3b85cb01f74e5). The interface header and five-file upstream patch derive from the collaborator's [original integration](https://github.com/geek-xiaoshuaiji/HBF-EventLab/tree/8fb8842e76cffed4ec9d268a3451e4c4a872803e/integration/accel_sim). The placement and write-coverage helpers retain their upstream notices in `third_party/hbf_sim/`.

See the [GPT-2 experiment](../../experiments/simulated_gpu/gpt2/README.md) for the workload, configuration and replay checks.
