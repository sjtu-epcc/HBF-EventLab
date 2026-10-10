#include "hbf_eventlab/runtime/zone_mapping.hpp"
#include <utility>

namespace hbf_eventlab {
struct MappingState {
    struct Zone {
        std::uint64_t target = 0, in_flight = 0;
        bool discardable = false;
        std::optional<std::uint64_t> quiesced_with;
    };
    struct Channel { std::vector<Zone> zones; Generation generation; };
    std::uint64_t zone_dlus, channel_bytes;
    std::vector<Channel> channels;
    Zone& zone(ChannelId ch, std::uint64_t z) {
        if (ch >= channels.size() || z >= channels[ch].zones.size()) throw std::out_of_range("zone outside channel");
        return channels[ch].zones[static_cast<std::size_t>(z)];
    }
    std::uint64_t zone_of(ChannelAddress a) {
        if (a.channel >= channels.size() || a.local.value >= channel_bytes) throw std::out_of_range("local address outside channel");
        return a.local.value / dlu_bytes / zone_dlus;
    }
};
MappingLease::MappingLease(std::shared_ptr<MappingState> s, ChannelId ch, std::uint64_t z,
        MediaAddress media, Generation gen)
    : state_(std::move(s)), channel_(ch), zone_(z), media_(media), generation_(gen) {}
MappingLease::MappingLease(MappingLease&& other) noexcept
    : state_(std::move(other.state_)), channel_(other.channel_), zone_(other.zone_),
      media_(other.media_), generation_(other.generation_) {}
MappingLease::~MappingLease() {
    if (state_) --state_->channels[channel_].zones[static_cast<std::size_t>(zone_)].in_flight;
}
ZoneMapping::ZoneMapping(const StaticDeviceDescription& d) : state_(std::make_shared<MappingState>()) {
    state_->zone_dlus = d.configuration().zone_size_dlus;
    state_->channel_bytes = d.geometry().channel_bytes();
    state_->channels.resize(d.geometry().host_channels);
    const auto count = d.geometry().channel_dlus() / state_->zone_dlus;
    for (auto& ch : state_->channels) {
        ch.zones.resize(static_cast<std::size_t>(count));
        for (std::uint64_t z = 0; z < count; ++z) ch.zones[static_cast<std::size_t>(z)].target = z;
    }
}
MediaAddress ZoneMapping::translate(ChannelAddress a) const {
    const auto z = state_->zone_of(a);
    const auto local_dlu = a.local.value / dlu_bytes;
    return {a.channel, MediaDlu{state_->zone(a.channel, z).target * state_->zone_dlus + local_dlu % state_->zone_dlus},
            static_cast<std::uint32_t>(a.local.value % dlu_bytes)};
}
Generation ZoneMapping::generation(ChannelId ch) const { return state_->channels.at(ch).generation; }
std::optional<MappingLease> ZoneMapping::pin(ChannelAddress a) {
    require_unmanaged(); return pin_impl(a);
}
bool ZoneMapping::begin_logical_use(ChannelAddress a) {
    auto& zone=state_->zone(a.channel,state_->zone_of(a));
    if (zone.quiesced_with) return false;
    zone.discardable=false;
    return true;
}
std::optional<MappingLease> ZoneMapping::pin_impl(ChannelAddress a, bool existing_logical_reader) {
    const auto z = state_->zone_of(a);
    auto& zone = state_->zone(a.channel, z);
    if (zone.quiesced_with && !existing_logical_reader) return std::nullopt;
    zone.in_flight = checked_add(zone.in_flight, 1);
    if (!existing_logical_reader) zone.discardable = false;
    return MappingLease{state_, a.channel, z, translate(a), generation(a.channel)};
}
void ZoneMapping::declare_discardable(ChannelId ch, std::uint64_t z) {
    require_unmanaged(); declare_discardable_impl(ch, z);
}
void ZoneMapping::declare_discardable_impl(ChannelId ch, std::uint64_t z) {
    // Host-supplied semantic liveness; not inferred from NAND valid bits.
    state_->zone(ch, z).discardable = true;
}
bool ZoneMapping::quiesce_pair(ChannelId ch, std::uint64_t a, std::uint64_t b) {
    require_unmanaged(); return quiesce_pair_impl(ch, a, b);
}
bool ZoneMapping::quiesce_pair_impl(ChannelId ch, std::uint64_t a, std::uint64_t b) {
    auto& x = state_->zone(ch, a); auto& y = state_->zone(ch, b);
    if (a == b) throw std::invalid_argument("swap requires distinct logical zones");
    if (x.quiesced_with || y.quiesced_with) return false;
    x.quiesced_with = b; y.quiesced_with = a;
    return true;
}
void ZoneMapping::cancel_quiesce(ChannelId ch, std::uint64_t a, std::uint64_t b) {
    require_unmanaged(); cancel_quiesce_impl(ch, a, b);
}
void ZoneMapping::cancel_quiesce_impl(ChannelId ch, std::uint64_t a, std::uint64_t b) {
    auto& x = state_->zone(ch, a); auto& y = state_->zone(ch, b);
    if (x.quiesced_with != std::optional<std::uint64_t>{b} || y.quiesced_with != std::optional<std::uint64_t>{a})
        throw std::logic_error("not this quiesce pair");
    x.quiesced_with.reset(); y.quiesced_with.reset();
}
RemapResult ZoneMapping::commit_swap(ChannelId ch, std::uint64_t a, std::uint64_t b,
        const std::function<void()>& invalidate) {
    require_unmanaged(); return commit_swap_impl(ch, a, b, invalidate);
}
RemapResult ZoneMapping::swap_status(ChannelId ch, std::uint64_t a, std::uint64_t b) const {
    const auto& x = state_->zone(ch, a); const auto& y = state_->zone(ch, b);
    if (a == b || x.quiesced_with != std::optional<std::uint64_t>{b} || y.quiesced_with != std::optional<std::uint64_t>{a})
        return RemapResult::NotQuiesced;
    if (x.in_flight || y.in_flight) return RemapResult::InFlight;
    if (!x.discardable || !y.discardable) return RemapResult::NotDiscardable;
    return RemapResult::Applied;
}
void ZoneMapping::require_unmanaged() const {
    if (managed_) throw std::logic_error("use CapacityRetirementCoordinator for managed mappings");
}
bool ZoneMapping::has_pins() const {
    for (const auto& ch : state_->channels) for (const auto& z : ch.zones)
        if (z.in_flight) return true;
    return false;
}
bool ZoneMapping::quiesced(ChannelAddress a) const {
    return state_->zone(a.channel, state_->zone_of(a)).quiesced_with.has_value();
}
RemapResult ZoneMapping::commit_swap_impl(ChannelId ch, std::uint64_t a, std::uint64_t b,
        const std::function<void()>& invalidate) {
    auto& x = state_->zone(ch, a); auto& y = state_->zone(ch, b);
    if (a == b || x.quiesced_with != std::optional<std::uint64_t>{b} || y.quiesced_with != std::optional<std::uint64_t>{a})
        return RemapResult::NotQuiesced;
    if (x.in_flight || y.in_flight) return RemapResult::InFlight;
    if (!x.discardable || !y.discardable) return RemapResult::NotDiscardable;
    if (!invalidate) throw std::invalid_argument("cache invalidation acknowledgment is required");
    const Generation next{checked_add(generation(ch).value, 1)};
    invalidate();
    std::swap(x.target, y.target); // SPEC swap CURRENT mappings, not literal zone IDs.
    state_->channels[ch].generation = next;
    x.quiesced_with.reset(); y.quiesced_with.reset();
    return RemapResult::Applied;
}
std::uint64_t ZoneMapping::mapped_zone(ChannelId ch, std::uint64_t z) const { return state_->zone(ch, z).target; }
} // namespace hbf_eventlab
