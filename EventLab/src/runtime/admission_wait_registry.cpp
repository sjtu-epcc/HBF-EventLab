#include "hbf_eventlab/runtime/admission_wait_registry.hpp"

namespace hbf_eventlab {
AdmissionWaitRegistry::~AdmissionWaitRegistry() {
    for (const auto& w : waits_) if (w.second.event) {
        if(profile_)++profile_->wait(w.second.trigger).cancelled;
        events_.cancel(*w.second.event);
    }
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
void AdmissionWaitRegistry::queue(WaitId id,WaitReason reason) {
    auto& w = waits_.at(id);
    if (w.event) return;
    w.trigger=reason;
    if(profile_)++profile_->wait(reason).queued;
    w.event = events_.schedule_at(events_.now(),EventPhase::Arrival,[this,id] {
        auto it = waits_.find(id);
        if (it == waits_.end()) return;
        const auto reason=it->second.trigger;
        RuntimeScope timing(profile_,RuntimeRegion::WaitCallback,reason);
        if(profile_)++profile_->wait(reason).callbacks;
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
    auto changed = WaitReason::Count;
    for (const auto& o : observations) {
        if(changed==WaitReason::Count && observe(o.key).revision != o.revision)changed=o.key.reason;
        if (index_[o.key].insert(id).second) w.keys.push_back(o.key);
    }
    if(profile_)profile_->peak(RuntimeCounter::WaitsPeak,waits_.size());
    if (changed!=WaitReason::Count) { if(profile_)++profile_->wait(changed).revision_wakes;queue(id,changed); }
    return id;
}
bool AdmissionWaitRegistry::cancel(WaitId id) {
    auto it = waits_.find(id); if (it == waits_.end()) return false;
    if (it->second.event) { if(profile_)++profile_->wait(it->second.trigger).cancelled;events_.cancel(*it->second.event); --queued_; }
    disarm(id,it->second); waits_.erase(it); return true;
}
void AdmissionWaitRegistry::notify(WaitKey key) {
    RuntimeScope timing(profile_,RuntimeRegion::WaitNotify);
    if(profile_)++profile_->wait(key.reason).notifications;
    auto& revision = revisions_[key]; revision = checked_add(revision,1);
    const auto it = index_.find(key); if (it == index_.end()) return;
    if(profile_)profile_->wait(key.reason).subscriber_hits+=it->second.size();
    const auto ids = it->second; // Only this object's subscribers; no global request scan.
    for (const auto id : ids) queue(id,key.reason);
}
} // namespace hbf_eventlab

