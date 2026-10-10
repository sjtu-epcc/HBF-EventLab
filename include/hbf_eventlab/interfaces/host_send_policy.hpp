#pragma once
#include "hbf_eventlab/host/workload.hpp"
namespace hbf_eventlab {
struct HostSendCandidate { SendUnitId id; bool supplements_open_dlu; bool replay; std::uint64_t sequence; };
class IHostSendPolicy {
public:
    virtual ~IHostSendPolicy()=default;
    virtual SendUnitId select(const std::vector<HostSendCandidate>&) const=0;
};
} // namespace hbf_eventlab

