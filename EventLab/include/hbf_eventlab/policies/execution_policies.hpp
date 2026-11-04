#pragma once
#include "hbf_eventlab/interfaces/execution_policy.hpp"
namespace hbf_eventlab {
class FcfsNandPolicy final : public INandSchedulingPolicy {
public:
    std::uint64_t select(const std::vector<MediaCandidateView>& c) const override {
        if(c.empty()) throw std::logic_error("empty NAND candidate set");
        return std::min_element(c.begin(),c.end(),[](const auto& a,const auto& b) {
            return std::tie(a.sequence,a.identity)<std::tie(b.sequence,b.identity);
        })->identity;
    }
};
// Non-preemptive; Device supplies only legal candidates. Sustained reads can
// starve writes. Age/identity break ties, including between program and erase.
class ReadPriorityNandPolicy final : public INandSchedulingPolicy {
public:
    std::uint64_t select(const std::vector<MediaCandidateView>& c) const override {
        if(c.empty()) throw std::logic_error("empty NAND candidate set");
        return std::min_element(c.begin(),c.end(),[](const auto& a,const auto& b) {
            return std::make_tuple(a.kind!=MediaOperationKind::Read,a.sequence,a.identity)<
                   std::make_tuple(b.kind!=MediaOperationKind::Read,b.sequence,b.identity);
        })->identity;
    }
};
class FifoUciePolicy final : public IUcieArbitrationPolicy {
public:
    std::uint64_t select(const std::vector<TransferCandidateView>& c) const override {
        if(c.empty()) throw std::logic_error("empty link candidate set");
        return std::min_element(c.begin(),c.end(),[](const auto& a,const auto& b) {
            return std::tie(a.sequence,a.identity)<std::tie(b.sequence,b.identity);
        })->identity;
    }
};
} // namespace hbf_eventlab
