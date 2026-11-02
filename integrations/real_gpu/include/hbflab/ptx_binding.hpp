#pragma once
#include "hbflab/protocol.hpp"
#include "hbflab/ptx.hpp"

namespace hbflab {
// Separate helper binding ABI; mailbox ABI v2 remains unchanged.
struct PtxBinding {
    DeviceView view{};
    std::uint64_t output_base = 0, output_bytes = 0, instance = 0;
    std::uint32_t frontend = ptx_frontend_version, protocol = abi_version;
};
static_assert(std::is_standard_layout<PtxBinding>::value && sizeof(PtxBinding) == 80);
}
