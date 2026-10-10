#pragma once
#include "hbf_eventlab/architecture/device_description.hpp"
#include <vector>

namespace hbf_eventlab {
struct BufferCapacity { std::size_t entries=16, payload_bytes=16384; };
struct ExecutionConfiguration {
    // One Tick = 1 ps throughout this execution profile (including assembly_timeout).
    Tick sense{100000}, program{500000}, erase{1000000}, cache_access{100};
    Tick read_window{10000}, request_fixed_latency{100}, response_fixed_latency{100};
    std::size_t read_threshold=32;
    // Effective GB/s (§5.1.2: 48/94/188); control byte equivalents are model assumptions.
    std::uint64_t effective_gbytes_per_second=48, request_control_bytes=16, response_control_bytes=16;
    BufferCapacity ingress{16,16384}, egress{8,512};
    std::uint32_t core_channels_per_ucie=1;
    // die -> Core Channel; empty means die % core_channels_per_ucie.
    std::vector<std::uint32_t> die_to_core_channel;
    void validate(const StaticDeviceDescription&) const;
    Tick serialization(std::uint64_t service_bytes) const;
};
class CoreChannelTopology {
public:
    CoreChannelTopology(const Geometry&,const ExecutionConfiguration&);
    std::uint32_t core_channel(std::uint32_t die) const;
    std::uint32_t count() const { return count_; }
    // Ideal link: zero delay and no occupancy. It NEVER multiplies die count/capacity.
private:
    std::uint32_t count_;
    std::vector<std::uint32_t> route_;
};
} // namespace hbf_eventlab

