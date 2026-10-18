#pragma once
#include <algorithm>
#include "hbf_eventlab/interfaces/host_send_policy.hpp"
#include <memory>
#include <tuple>
namespace hbf_eventlab {
class FcfsHostSendPolicy final:public IHostSendPolicy {
public:
    SendUnitId select(const std::vector<HostSendCandidate>& c) const override {
        if(c.empty())throw std::logic_error("empty Host candidate set");
        return std::min_element(c.begin(),c.end(),[](const auto& a,const auto& b) {
            return std::tie(a.sequence,a.id)<std::tie(b.sequence,b.id);
        })->id;
    }
};
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
inline void validate_host_send_policy(const std::string& name) {
    if(name!="fcfs" && name!="fill_open")
        throw std::invalid_argument("host.send_policy: unsupported policy '"+name+"'; supported: fcfs, fill_open");
}
inline std::unique_ptr<IHostSendPolicy> make_host_send_policy(const std::string& name) {
    validate_host_send_policy(name);
    if(name=="fcfs")return std::make_unique<FcfsHostSendPolicy>();
    return std::make_unique<FillOpenDluPolicy>();
}
} // namespace hbf_eventlab

