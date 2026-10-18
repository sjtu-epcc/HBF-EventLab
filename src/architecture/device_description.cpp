#include "hbf_eventlab/architecture/device_description.hpp"

namespace hbf_eventlab {
namespace {
bool legal_mocs(std::uint32_t n) {
    return n == 1 || (n >= 256 && n <= 16384 && (n & (n - 1)) == 0);
}
}
std::uint64_t Geometry::lanes() const { return checked_mul(dies_per_channel, banks_per_die); }
std::uint64_t Geometry::block_group_dlus() const { return checked_mul(lanes(), pages_per_block); }
std::uint64_t Geometry::channel_dlus() const { return checked_mul(block_group_dlus(), blocks_per_bank); }
std::uint64_t Geometry::channel_bytes() const { return checked_mul(channel_dlus(), dlu_bytes); }
void CapacityPolicy::validate() const {
    if(max_hole_permille>1000)
        throw std::invalid_argument("device.capacity_policy.max_hole_permille: must be in [0, 1000]");
}
void Geometry::validate() const {
    if (!host_channels || host_channels>16) throw std::invalid_argument("device.geometry.host_channels: must be in [1, 16], also subject to speed grade");
    if (!dies_per_channel || dies_per_channel>16 || (dies_per_channel & (dies_per_channel-1)))
        throw std::invalid_argument("device.geometry.dies_per_channel: expected 1, 2, 4, 8, or 16, also subject to speed grade");
    if (!banks_per_die) throw std::invalid_argument("device.geometry.banks_per_die: must be positive");
    if (!pages_per_block) throw std::invalid_argument("device.geometry.pages_per_block: must be positive");
    if (!blocks_per_bank) throw std::invalid_argument("device.geometry.blocks_per_bank: must be positive");
    (void)checked_mul(channel_bytes(), host_channels);
    (void)physical_blocks_per_bank();
}
StaticDeviceDescription::StaticDeviceDescription(Geometry g, Capabilities caps, DeviceOperatingConfiguration config,
        CoreChannelTopologyConfiguration topology, CapacityPolicy capacity)
    : topology_(std::move(topology)), capacity_policy_(capacity), geometry_(g), capabilities_(caps), configuration_(config) {
    g.validate(); topology_.validate(g); capacity_policy_.validate();
    if (!legal_mocs(caps.mocs_supported))
        throw std::invalid_argument("device.capabilities.mocs_supported: expected 1 or a power of two in [256, 16384]");
    if (!legal_mocs(config.mocs_selected))
        throw std::invalid_argument("device.selected.mocs_selected: expected 1 or a power of two in [256, 16384]");
    if(config.mocs_selected>caps.mocs_supported)
        throw std::invalid_argument("device.selected.mocs_selected: must not exceed device.capabilities.mocs_supported");
    if (!caps.max_pending_write_dlus) throw std::invalid_argument("device.capabilities.max_pending_write_dlus: must be positive");
    if (!config.assembly_timeout.value) throw std::invalid_argument("device.selected.assembly_timeout_ps: must be positive");
    // BASELINE stronger alignment; OCP only states a NAND-block-size multiple.
    if (!config.zone_size_dlus || config.zone_size_dlus % g.block_group_dlus() ||
        g.channel_dlus() % config.zone_size_dlus)
        throw std::invalid_argument("device.selected.zone_size_dlus: current layout requires whole block groups that tile the channel");
    // Progress depends on the actual workload and Host write mode. A legal small
    // MOCS remains runnable (read-only, full burst, or intentional timeout study).
}
StaticDeviceDescription demonstration_profile() {
    Geometry g{2, 4, 16, 8, 8, 2};
    return {g, {256, 4, false, true}, {256, Tick{100}, g.block_group_dlus()}};
}
} // namespace hbf_eventlab
