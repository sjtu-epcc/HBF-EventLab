#pragma once
#include "hbflab/protocol.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace hbflab {
// Checked rational conversion; no overflowing intermediate multiplication.
inline std::uint64_t scale_ticks(std::uint64_t value, std::uint32_t mul,
                                std::uint32_t div, bool round_up = false) {
    if (!mul || !div) throw std::invalid_argument("invalid time scale");
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    const auto whole = value / div;
    const auto remainder = (value % div) * std::uint64_t{mul};
    const auto tail = remainder / div + (round_up && remainder % div != 0);
    if (whole > (limit - tail) / mul) throw std::overflow_error("time conversion overflow");
    return whole * mul + tail;
}
struct FrontierItem {
    std::uint32_t producer;
    std::uint64_t ticket, arrival_ns, arrival_ps;
};
// Input is slot order, not polling order. Caller supplies a closed frontier.
inline std::vector<FrontierItem> order_frontier(const std::vector<Request>& requests,
    std::uint64_t ticket, std::uint64_t gpu_epoch, std::uint64_t model_epoch,
    std::uint64_t model_now, std::uint32_t scale) {
    if (requests.empty() || requests.size() > max_threads || !ticket)
        throw std::invalid_argument("invalid frontier size/ticket");
    std::vector<FrontierItem> result;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];
        if (r.version != abi_version || r.producer != i || r.ticket != ticket || r.arrival_ns < gpu_epoch)
            throw std::invalid_argument("frontier ownership, ticket or timestamp mismatch");
        const auto delta = scale_ticks(r.arrival_ns - gpu_epoch, 1000, scale);
        if (delta > UINT64_MAX - model_epoch) throw std::overflow_error("arrival overflow");
        const auto at = model_epoch + delta;
        if (at < model_now) throw std::invalid_argument("late frontier would rewind simulated time");
        result.push_back({static_cast<std::uint32_t>(i), ticket, r.arrival_ns, at});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.arrival_ns, a.producer, a.ticket) < std::tie(b.arrival_ns, b.producer, b.ticket);
    });
    return result;
}
}
