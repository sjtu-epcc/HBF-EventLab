#include "hbf_eventlab/architecture/execution_configuration.hpp"
namespace hbf_eventlab {
void ExecutionConfiguration::validate(const StaticDeviceDescription& d) const {
    if(!sense.value || !program.value || !erase.value || !cache_access.value || !effective_gbytes_per_second ||
       !request_control_bytes || !response_control_bytes || !ingress.entries || !egress.entries ||
       egress.payload_bytes<host_unit_bytes || ingress.payload_bytes<(d.capabilities().write_burst?dlu_bytes:host_unit_bytes))
        throw std::invalid_argument("invalid timing/buffer profile; RX must fit advertised largest transaction");
    if(!core_channels_per_ucie || core_channels_per_ucie>d.geometry().dies_per_channel)
        throw std::invalid_argument("invalid Core Channel count");
    if(!die_to_core_channel.empty() && die_to_core_channel.size()!=d.geometry().dies_per_channel)
        throw std::invalid_argument("Core route must name every existing die exactly once");
    for(auto ch:die_to_core_channel) if(ch>=core_channels_per_ucie) throw std::invalid_argument("Core route out of range");
}
Tick ExecutionConfiguration::serialization(std::uint64_t bytes) const {
    if(!effective_gbytes_per_second) throw std::invalid_argument("zero link bandwidth");
    const auto ps=checked_mul(bytes,1000);
    return Tick{ps/effective_gbytes_per_second+(ps%effective_gbytes_per_second!=0)};
}
CoreChannelTopology::CoreChannelTopology(const Geometry& g,const ExecutionConfiguration& c)
    :count_(c.core_channels_per_ucie),route_(c.die_to_core_channel) {
    if(!count_ || count_>g.dies_per_channel || (!route_.empty() && route_.size()!=g.dies_per_channel))
        throw std::invalid_argument("invalid Core Channel topology");
    for(auto core:route_) if(core>=count_) throw std::invalid_argument("invalid Core route");
    if(route_.empty()) for(std::uint32_t die=0;die<g.dies_per_channel;++die) route_.push_back(die%count_);
}
std::uint32_t CoreChannelTopology::core_channel(std::uint32_t die) const { return route_.at(die); }
}

