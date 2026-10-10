#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <vector>
#include <algorithm>

namespace hbf_eventlab {
enum class MediaOperationKind { Read, Program, Erase };
struct MediaCandidateView {
    std::uint64_t identity, sequence;
    MediaOperationKind kind;
    PhysicalNandAddress target;
};
// Input contains ONLY currently legal candidates. Policy returns an input identity;
// Device revalidates and commits resource/path state. Selection has no side effects.
class INandSchedulingPolicy {
public:
    virtual ~INandSchedulingPolicy() = default;
    virtual std::uint64_t select(const std::vector<MediaCandidateView>&) const = 0;
};
struct TransferCandidateView { std::uint64_t identity, sequence; Op op; std::uint64_t service_bytes; };
class IUcieArbitrationPolicy {
public:
    virtual ~IUcieArbitrationPolicy() = default;
    virtual std::uint64_t select(const std::vector<TransferCandidateView>&) const = 0;
};
} // namespace hbf_eventlab

