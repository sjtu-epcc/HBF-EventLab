#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <map>
#include <optional>

namespace hbf_eventlab {
using LogicalReadId = Quantity<struct LogicalReadTag>;
// Index of unsatisfied logical content dependencies. No physical pins or payloads.
// Owned by CapacityRetirementCoordinator; ReadPath acquires/releases entries.
class LogicalAccessTracker {
public:
    LogicalReadId acquire(LogicalNandAddress address, Generation contents) {
        const LogicalReadId id{checked_add(next_,1)};
        entries_.emplace(id, Entry{address,contents});
        ++blocks_[address.block][address.page]; next_ = id.value; return id;
    }
    LogicalBlockKey release(LogicalReadId id) {
        const auto it = entries_.find(id);
        if (it == entries_.end()) throw std::logic_error("stale logical read guard");
        const auto a = it->second.address;
        auto& pages = blocks_.at(a.block);
        if (--pages.at(a.page) == 0) pages.erase(a.page);
        if (pages.empty()) blocks_.erase(a.block);
        entries_.erase(it); return a.block;
    }
    bool needs_block(LogicalBlockKey b) const { return blocks_.count(b) != 0; }
    bool matches(LogicalReadId id, LogicalNandAddress a, Generation g) const {
        const auto it=entries_.find(id);
        return it!=entries_.end() && it->second.address.block==a.block &&
               it->second.address.page==a.page && it->second.contents==g;
    }
    bool needs_page(LogicalBlockKey b, PageId p) const {
        const auto it = blocks_.find(b); return it != blocks_.end() && it->second.count(p);
    }
    std::size_t size() const { return entries_.size(); }
private:
    struct Entry { LogicalNandAddress address; Generation contents; };
    std::map<LogicalReadId,Entry> entries_;
    std::map<LogicalBlockKey,std::map<PageId,std::size_t>> blocks_;
    std::uint64_t next_ = 0;
};
} // namespace hbf_eventlab
