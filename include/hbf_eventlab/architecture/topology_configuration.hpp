#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <vector>
namespace hbf_eventlab {
struct Geometry;
struct CoreChannelTopologyConfiguration {
    std::uint32_t core_channels_per_ucie=1;
    // Empty selects die % core_channels_per_ucie; routes never add dies/capacity.
    std::vector<std::uint32_t> die_to_core_channel;
    void validate(const Geometry&) const;
    std::vector<std::uint32_t> resolved_routes(const Geometry&) const;
};
class CoreChannelTopology {
public:
    CoreChannelTopology(const Geometry&,const CoreChannelTopologyConfiguration&);
    std::uint32_t core_channel(std::uint32_t die) const { return route_.at(die); }
    std::uint32_t count() const { return count_; }
    // Current model: ideal Core links, zero delay and no bandwidth contention.
private:
    std::uint32_t count_;
    std::vector<std::uint32_t> route_;
};
}
