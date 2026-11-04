#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <map>
#include <optional>

namespace hbf_eventlab {
class CapacityRetirementCoordinator;
// Sparse final physical targets. No entry ever points to another entry.
// Keys are post-zone nominal slots; logical retirement is checked before lookup.
class RetiredHoleMap {
public:
    using Entries = std::map<BlockSlotKey, PhysicalBlockKey>;
    std::optional<PhysicalBlockKey> lookup(BlockSlotKey slot) const {
        auto it = entries_.find(slot);
        return it == entries_.end() ? std::nullopt : std::optional<PhysicalBlockKey>{it->second};
    }
    const Entries& entries() const { return entries_; }
    std::size_t size(ChannelId channel) const {
        std::size_t n = 0;
        for (const auto& e : entries_) if (e.first.channel == channel) ++n;
        return n;
    }
private:
    friend class CapacityRetirementCoordinator;
    void commit(Entries& staged) noexcept { entries_.swap(staged); }
    Entries entries_;
};
} // namespace hbf_eventlab
