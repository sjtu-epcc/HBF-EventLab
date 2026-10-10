#pragma once
#include "hbf_eventlab/interfaces/address.hpp"

namespace hbf_eventlab {
class RoundRobinInterleaver final : public IHostInterleaver {
public:
    RoundRobinInterleaver(GlobalByteAddress base, std::uint32_t channels,
                         std::uint64_t channel_bytes, std::uint64_t split_bytes = host_unit_bytes);
    ChannelAddress map(GlobalByteAddress) const override;
private:
    GlobalByteAddress base_;
    std::uint32_t channels_;
    std::uint64_t split_bytes_, total_bytes_;
};
} // namespace hbf_eventlab
