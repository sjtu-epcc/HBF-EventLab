#pragma once
#include "hbf_eventlab/infrastructure/event_queue.hpp"
#include "hbf_eventlab/diagnostics/runtime_profile.hpp"
#include <map>

namespace hbf_eventlab {
using WaitId = Quantity<struct WaitTag>;
struct WaitKey {
    WaitReason reason;
    ChannelId channel = 0;
    std::uint32_t die = 0, bank = 0;
    std::uint64_t object = 0;
    static WaitKey channel_key(WaitReason r, ChannelId ch) { return {r,ch,0,0,0}; }
    static WaitKey block_key(WaitReason r, LogicalBlockKey b) { return {r,b.channel,b.die,b.bank,b.block}; }
    static WaitKey physical(PhysicalBlockKey b) { return {WaitReason::PhysicalUsers,b.channel,b.die,b.bank,b.block}; }
    static WaitKey physical_state(PhysicalBlockKey b) { return {WaitReason::PhysicalState,b.channel,b.die,b.bank,b.block}; }
    static WaitKey dlu(DluKey k) { return {WaitReason::DluProgress,k.channel,0,0,k.dlu.value}; }
    // Zone denotes admission/mapping state; ZoneUsers denotes draining progress.
    static WaitKey zone(ChannelId ch, std::uint64_t z) { return {WaitReason::Zone,ch,0,0,z}; }
    static WaitKey zone_users(ChannelId ch, std::uint64_t z) { return {WaitReason::ZoneUsers,ch,0,0,z}; }
    static WaitKey order(ChannelId ch, std::uint32_t group) { return {WaitReason::ReadOrder,ch,0,0,group}; }
    friend bool operator<(const WaitKey& a, const WaitKey& b) {
        return std::tie(a.reason,a.channel,a.die,a.bank,a.object) < std::tie(b.reason,b.channel,b.die,b.bank,b.object);
    }
};
struct WaitObservation { WaitKey key; std::uint64_t revision; };
// One-shot, indexed, single simulator thread. State owners notify after mutation;
// subscribers always run in a later event, never from a destructor/commit callback.
class AdmissionWaitRegistry {
public:
    explicit AdmissionWaitRegistry(EventQueue& events) : events_(events) {}
    ~AdmissionWaitRegistry();
    AdmissionWaitRegistry(const AdmissionWaitRegistry&) = delete;
    AdmissionWaitRegistry& operator=(const AdmissionWaitRegistry&) = delete;
    WaitObservation observe(WaitKey) const;
    // Observe BEFORE checking; changed revision prevents check/register lost wakeups.
    WaitId subscribe(const std::vector<WaitObservation>&, std::function<void()>);
    bool cancel(WaitId);
    void notify(WaitKey);
    void set_runtime_profile(RuntimeProfile& p) { if(profile_ && profile_!=&p) throw std::logic_error("notification profile already attached"); profile_=&p; }
    RuntimeProfile* runtime_profile() const { return profile_; }
    std::size_t pending() const { return waits_.size(); }
    std::size_t queued_wakeups() const { return queued_; }
    EventQueue& events() const { return events_; }
private:
    struct Wait { std::vector<WaitKey> keys; std::function<void()> callback; std::optional<EventId> event; WaitReason trigger=WaitReason::Count; };
    void disarm(WaitId,Wait&);
    void queue(WaitId,WaitReason);
    EventQueue& events_;
    RuntimeProfile* profile_=nullptr;
    std::uint64_t next_id_ = 0;
    std::size_t queued_ = 0;
    std::map<WaitKey,std::uint64_t> revisions_;
    std::map<WaitKey,std::set<WaitId>> index_;
    std::map<WaitId,Wait> waits_;
};
} // namespace hbf_eventlab

