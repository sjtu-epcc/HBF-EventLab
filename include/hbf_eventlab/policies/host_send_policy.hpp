#pragma once
#include <algorithm>
#include "hbf_eventlab/interfaces/host_send_policy.hpp"
#include <algorithm>
namespace hbf_eventlab {
class FillOpenDluPolicy final:public IHostSendPolicy {
public:
    SendUnitId select(const std::vector<HostSendCandidate>& c) const override {
        if(c.empty())throw std::logic_error("empty Host candidate set");
        return std::min_element(c.begin(),c.end(),[](const auto&a,const auto&b) {
            return std::make_tuple(!a.supplements_open_dlu,!a.replay,a.sequence,a.id) <
                   std::make_tuple(!b.supplements_open_dlu,!b.replay,b.sequence,b.id);
        })->id;
    }
};
} // namespace hbf_eventlab

