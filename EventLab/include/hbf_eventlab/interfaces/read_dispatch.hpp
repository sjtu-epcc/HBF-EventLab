#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <cstddef>

namespace hbf_eventlab {
struct ReadWindowView { std::size_t size; Tick first_arrival; };
enum class ReadReleaseReason { Batch, Timer, Threshold, Immediate, Policy };
const char* to_string(ReadReleaseReason);
class IReadDispatchPolicy {
public:
    virtual ~IReadDispatchPolicy() = default;
    virtual bool ready(ReadWindowView, Tick now) const = 0;
    // The driver MUST schedule this wakeup even if no more requests arrive.
    virtual Tick deadline(ReadWindowView) const = 0;
    // Diagnostic only, called after ready() succeeds. No effect on eligibility.
    virtual ReadReleaseReason release_reason(ReadWindowView, Tick) const { return ReadReleaseReason::Policy; }
};
} // namespace hbf_eventlab
