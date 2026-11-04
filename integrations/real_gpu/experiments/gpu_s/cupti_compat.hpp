#pragma once
#include <cupti.h>
#include <cstdint>
#include <stdexcept>
#ifndef GPU_S_CUPTI_KERNEL_RECORD
#error "Configure the S experiment with CMake to select the CUPTI record ABI"
#endif
using GpuSKernelActivity = GPU_S_CUPTI_KERNEL_RECORD;
inline void verify_cupti_abi() {
    std::uint32_t version = 0;
    if (cuptiGetVersion(&version) != CUPTI_SUCCESS || version != CUPTI_API_VERSION)
        throw std::runtime_error("CUPTI runtime/header API version mismatch");
}
