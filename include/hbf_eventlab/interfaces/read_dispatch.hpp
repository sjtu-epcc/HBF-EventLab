#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <cstddef>

namespace hbf_eventlab {
struct ReadWindowView { std::size_t size; Tick first_arrival; };
class IReadDispatchPolicy {
public:
    virtual ~IReadDispatchPolicy() = default;
    virtual bool ready(ReadWindowView, Tick now) const = 0;
    // The driver MUST schedule this wakeup even if no more requests arrive.
    virtual Tick deadline(ReadWindowView) const = 0;
};
} // namespace hbf_eventlab
