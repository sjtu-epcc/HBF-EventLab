#include "hbflab/device.cuh"
#include "hbflab/ptx_binding.hpp"

extern "C" {
__device__ __constant__ hbflab::PtxBinding __hbflab_binding;

// op=0: readonly timing gate; op=1: bounded result store, no HBF write model.
__device__ __noinline__ unsigned __hbflab_gate(unsigned long long address,
                                             unsigned bytes, unsigned active, unsigned op) {
    using namespace hbflab;
    const auto& b = __hbflab_binding;
    const auto v = b.view;
    if (!b.instance || b.frontend != ptx_frontend_version || b.protocol != abi_version)
        return 0; // Host validates the binding before any launch.
    if (!op) return DeviceClient(v).gate_load(reinterpret_cast<const void*>(address), bytes, active != 0) == Status::Ok;
    bool bad = false;
    if (active) {
        bad = !bytes || bytes > 16 || address % bytes || address < b.output_base ||
            b.output_bytes < bytes || address - b.output_base > b.output_bytes - bytes;
        for (unsigned i = 0; i < v.range_count && !bad; ++i) {
            const auto r = v.ranges[i];
            // Validated output interval cannot wrap; subtraction avoids range-end overflow.
            bad = address >= r.gpu_base ? address - r.gpu_base < r.bytes : r.gpu_base - address < bytes;
        }
    }
    const auto owner = v.mode == ProducerMode::Thread ? threadIdx.x : threadIdx.x / 32;
    const bool writer = v.mode == ProducerMode::Thread || threadIdx.x % 32 == 0;
    if (__syncthreads_or(bad) && writer)
        release(v.mailbox[owner].terminal, static_cast<unsigned>(Status::InvalidRange));
    __syncthreads();
    const bool failed = acquire(v.mailbox[owner].terminal) != 0;
    return !__syncthreads_or(failed);
}
}
