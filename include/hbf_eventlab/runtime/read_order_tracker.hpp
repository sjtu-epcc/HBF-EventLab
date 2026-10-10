#pragma once
#include "hbf_eventlab/common/host_protocol.hpp"
#include <deque>
#include <map>
#include <optional>

namespace hbf_eventlab {
using CommandKey = std::pair<ChannelId,CommandId>;
// Delivery ordering, not data storage. A group can contain different pages.
// Absent group means unordered; axi_id is not implicitly reinterpreted as a group.
class ReadOrderTracker {
public:
    void accept(CommandKey key, std::optional<ReadGroup> group) {
        if (!group) return;
        if (!membership_.emplace(key,*group).second) throw std::logic_error("duplicate ordered read");
        groups_[{key.first,*group}].push_back(key.second);
    }
    bool head(CommandKey key) const {
        const auto it = membership_.find(key);
        return it == membership_.end() || groups_.at({key.first,it->second}).front() == key.second;
    }
    std::optional<ReadGroup> delivered(CommandKey key) {
        if (!head(key)) throw std::logic_error("out-of-order read delivery");
        return erase(key);
    }
    // Shutdown only; not runtime cancellation.
    void abandon(CommandKey key) { erase(key); }
    std::size_t pending() const { return membership_.size(); }
private:
    std::optional<ReadGroup> erase(CommandKey key) {
        auto it = membership_.find(key); if (it == membership_.end()) return {};
        const auto g = it->second; auto& q = groups_.at({key.first,g});
        for (auto pos=q.begin();pos!=q.end();++pos) if (*pos==key.second) { q.erase(pos); break; }
        if (q.empty()) groups_.erase({key.first,g});
        membership_.erase(it); return g;
    }
    std::map<CommandKey,ReadGroup> membership_;
    std::map<std::pair<ChannelId,ReadGroup>,std::deque<CommandId>> groups_;
};
} // namespace hbf_eventlab
