#pragma once
#include "hbf_eventlab/common/types.hpp"

namespace hbf_eventlab {
struct Geometry {
    std::uint32_t host_channels, dies_per_channel, banks_per_die, pages_per_block;
    std::uint64_t blocks_per_bank;
    std::uint32_t spare_blocks_per_bank = 0; // Additional physical capacity, invisible to Host.
    void validate() const;
    std::uint64_t lanes() const;
    std::uint64_t block_group_dlus() const;
    std::uint64_t channel_dlus() const;
    std::uint64_t channel_bytes() const;
    std::uint64_t physical_blocks_per_bank() const { return checked_add(blocks_per_bank, spare_blocks_per_bank); }
};
struct Capabilities {
    std::uint32_t max_outstanding_commands, max_pending_write_dlus;
    bool read_burst, write_burst;
};
struct RunConfiguration {
    std::uint32_t outstanding_limit;
    Tick assembly_timeout;
    std::uint64_t zone_size_dlus;
};
// Frozen at construction: CSR/ADMIN and runtime configuration changes are deferred.
class StaticDeviceDescription {
public:
    StaticDeviceDescription(Geometry, Capabilities, RunConfiguration);
    const Geometry& geometry() const { return geometry_; }
    const Capabilities& capabilities() const { return capabilities_; }
    const RunConfiguration& configuration() const { return configuration_; }
private:
    Geometry geometry_;
    Capabilities capabilities_;
    RunConfiguration configuration_;
};
// Demonstrator parameters, NOT OCP defaults or calibrated SSD/HBF timing.
StaticDeviceDescription demonstration_profile();
} // namespace hbf_eventlab
