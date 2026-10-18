#include "hbf_eventlab/architecture/topology_configuration.hpp"
#include "hbf_eventlab/architecture/device_description.hpp"
namespace hbf_eventlab {
void CoreChannelTopologyConfiguration::validate(const Geometry& g) const {
    if(!core_channels_per_ucie || core_channels_per_ucie>g.dies_per_channel)
        throw std::invalid_argument("device.topology.core_channels_per_ucie: must be in [1, dies_per_channel]");
    if(!die_to_core_channel.empty() && die_to_core_channel.size()!=g.dies_per_channel)
        throw std::invalid_argument("device.topology.die_to_core_channel: requires one entry per die");
    for(auto ch:die_to_core_channel) if(ch>=core_channels_per_ucie)
        throw std::invalid_argument("device.topology.die_to_core_channel: Core Channel index outside configured range");
}
std::vector<std::uint32_t> CoreChannelTopologyConfiguration::resolved_routes(const Geometry& g) const {
    validate(g);
    auto routes=die_to_core_channel;
    if(routes.empty()) for(std::uint32_t die=0;die<g.dies_per_channel;++die) routes.push_back(die%core_channels_per_ucie);
    return routes;
}
CoreChannelTopology::CoreChannelTopology(const Geometry& g,const CoreChannelTopologyConfiguration& c)
    :count_(c.core_channels_per_ucie),route_(c.resolved_routes(g)) {}
}
