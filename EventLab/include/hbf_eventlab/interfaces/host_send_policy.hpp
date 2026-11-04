#pragma once
#include "hbf_eventlab/host/workload.hpp"
namespace hbf_eventlab {
// HostDriver filters legality before selection. sequence is this queue entry's
// age (renewed on real retry), NOT the stable SendUnitId or a Device acceptance ID.
// A blocked candidate may be bypassed; NotTaken does not renew its sequence.
struct HostSendCandidate { SendUnitId id; bool supplements_open_dlu; bool replay; std::uint64_t sequence; };
class IHostSendPolicy {
public:
    virtual ~IHostSendPolicy()=default;
    virtual SendUnitId select(const std::vector<HostSendCandidate>&) const=0;
};
} // namespace hbf_eventlab

