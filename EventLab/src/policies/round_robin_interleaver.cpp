#include "hbf_eventlab/policies/round_robin_interleaver.hpp"

namespace hbf_eventlab {
RoundRobinInterleaver::RoundRobinInterleaver(GlobalByteAddress base, std::uint32_t channels,
        std::uint64_t capacity, std::uint64_t split)
    : base_(base), channels_(channels), split_bytes_(split), total_bytes_(checked_mul(channels, capacity)) {
    if (!channels || !capacity || !split || split % host_unit_bytes || capacity % split || base.value % host_unit_bytes)
        throw std::invalid_argument("invalid round-robin interleave geometry");
    (void)checked_add(base.value, total_bytes_);
}
ChannelAddress RoundRobinInterleaver::map(GlobalByteAddress a) const {
    if (a.value < base_.value || a.value - base_.value >= total_bytes_)
        throw std::out_of_range("global address outside configured region");
    const auto offset = a.value - base_.value;
    const auto stripe = offset / split_bytes_;
    return {static_cast<ChannelId>(stripe % channels_),
            LocalByteAddress{(stripe / channels_) * split_bytes_ + offset % split_bytes_}};
}
} // namespace hbf_eventlab
