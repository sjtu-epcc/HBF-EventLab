#include "hbf_eventlab/runtime/admission_wait_registry.hpp"

namespace hbf_eventlab {
AdmissionWaitRegistry::~AdmissionWaitRegistry() {
    for (const auto& w : waits_) if (w.second.event) events_.cancel(*w.second.event);
}
WaitObservation AdmissionWaitRegistry::observe(WaitKey key) const {
    const auto it = revisions_.find(key);
    return {key,it == revisions_.end() ? 0 : it->second};
}
void AdmissionWaitRegistry::disarm(WaitId id,Wait& w) {
    for (const auto& key : w.keys) {
        auto it = index_.find(key);
        if (it != index_.end()) { it->second.erase(id); if (it->second.empty()) index_.erase(it); }
    }
    w.keys.clear();
}
void AdmissionWaitRegistry::queue(WaitId id) {
    auto& w = waits_.at(id);
    if (w.event) return;
    w.event = events_.schedule_at(events_.now(),EventPhase::Arrival,[this,id] {
        auto it = waits_.find(id);
        if (it == waits_.end()) return;
        auto callback = std::move(it->second.callback);
        --queued_;
        waits_.erase(it); // Remove before callback can register again.
        callback();
    });
    ++queued_;
    disarm(id,w);
}
WaitId AdmissionWaitRegistry::subscribe(const std::vector<WaitObservation>& observations,std::function<void()> cb) {
    if (observations.empty() || !cb) throw std::invalid_argument("wait needs conditions and callback");
    const WaitId id{checked_add(next_id_,1)};
    auto& w = waits_.emplace(id,Wait{{},std::move(cb),{}}).first->second;
    next_id_ = id.value;
    bool changed = false;
    for (const auto& o : observations) {
        changed |= observe(o.key).revision != o.revision;
        if (index_[o.key].insert(id).second) w.keys.push_back(o.key);
    }
    if (changed) queue(id);
    return id;
}
bool AdmissionWaitRegistry::cancel(WaitId id) {
    auto it = waits_.find(id); if (it == waits_.end()) return false;
    if (it->second.event) { events_.cancel(*it->second.event); --queued_; }
    disarm(id,it->second); waits_.erase(it); return true;
}
void AdmissionWaitRegistry::notify(WaitKey key) {
    auto& revision = revisions_[key]; revision = checked_add(revision,1);
    const auto it = index_.find(key); if (it == index_.end()) return;
    const auto ids = it->second; // Only this object's subscribers; no global request scan.
    for (const auto id : ids) queue(id);
}
} // namespace hbf_eventlab

