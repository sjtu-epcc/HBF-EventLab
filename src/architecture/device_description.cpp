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
void Geometry::validate() const {
    if (!host_channels || !dies_per_channel || !banks_per_die || !pages_per_block || !blocks_per_bank)
        throw std::invalid_argument("geometry dimensions must be positive");
    if (host_channels > 16 || dies_per_channel > 16 || (dies_per_channel & (dies_per_channel - 1)))
        throw std::invalid_argument("unsupported OCP-reference channel/die count");
    (void)checked_mul(channel_bytes(), host_channels);
    (void)physical_blocks_per_bank();
}
StaticDeviceDescription::StaticDeviceDescription(Geometry g, Capabilities caps, RunConfiguration config)
    : geometry_(g), capabilities_(caps), configuration_(config) {
    g.validate();
    if (!legal_mocs(caps.max_outstanding_commands) || !legal_mocs(config.outstanding_limit) ||
        config.outstanding_limit > caps.max_outstanding_commands)
        throw std::invalid_argument("invalid MOCS capability/selection");
    if (!caps.max_pending_write_dlus || !config.assembly_timeout.value)
        throw std::invalid_argument("pending limit and assembly timeout must be positive");
    // BASELINE stronger alignment; OCP only states a NAND-block-size multiple.
    if (!config.zone_size_dlus || config.zone_size_dlus % g.block_group_dlus() ||
        g.channel_dlus() % config.zone_size_dlus)
        throw std::invalid_argument("reference zones must cover whole block groups and tile the channel");
    // Model consistency, not a new OCP rule: credits last until host response.
    if (!caps.write_burst && config.outstanding_limit < fragments_per_dlu)
        throw std::invalid_argument("64B-command write model cannot complete a DLU with this MOCS");
}
StaticDeviceDescription demonstration_profile() {
    Geometry g{2, 4, 16, 8, 8, 2};
    return {g, {256, 4, false, true}, {256, Tick{100}, g.block_group_dlus()}};
}
} // namespace hbf_eventlab
