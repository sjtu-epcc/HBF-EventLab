#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <optional>
#include <vector>

namespace hbf_eventlab {
// Future policy seam, no automatic-WL algorithm is claimed or implemented.
// Wear belongs to PHYSICAL media; discardability comes from Host lifetime information.
struct ZoneObservation {
    ChannelId channel;
    std::uint64_t logical_zone, media_zone, maximum_physical_pec;
    bool host_declared_discardable, has_in_flight_io;
};
struct ZoneSwapDecision { ChannelId channel; std::uint64_t first, second; };
class IWearLevelingPolicy {
public:
    virtual ~IWearLevelingPolicy() = default;
    virtual std::optional<ZoneSwapDecision> choose(const std::vector<ZoneObservation>&) const = 0;
};
// A decision is only a proposal: ZoneMapping still enforces quiescence/liveness.
} // namespace hbf_eventlab
