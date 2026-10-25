#include "hbf_eventlab/host/host_driver.hpp"
#include <algorithm>

namespace hbf_eventlab {
HostStorage HostDriver::storage() const {
    return {requests_.size(),results_.size(),work_.size(),units_.size(),attempts_.size(),versions_.size(),retention_.size()};
}
void HostDriver::set_input_capacity_handler(std::function<void(InputCapacity)> handler) {
    if(loaded_||started_||input_capacity_handler_||!handler)
        throw std::logic_error("attach capacity handler once before input/start");
    input_capacity_handler_=std::move(handler);
}
InputAdmission HostDriver::try_submit(const std::vector<GlobalRequest>& input) {
    if(!incremental_||!started_)throw std::logic_error("submit requires start_incremental");
    return admit_input(input,false);
}
InputAdmission HostDriver::try_start_incremental(const std::vector<GlobalRequest>& input) {
    if(loaded_||started_)throw std::logic_error("Host input mode already selected");
    return admit_input(input,true);
}
InputAdmission HostDriver::admit_input(const std::vector<GlobalRequest>& input,bool initial) {
    std::uint64_t count=0;
    for(const auto& r:input) {
        if(!r.bytes||r.bytes%host_unit_bytes)throw std::invalid_argument("unaligned/empty Global range");
        count=checked_add(count,r.bytes/host_unit_bytes);
    }
    // Do not materialize an arbitrarily large rejected range. TooLarge is not a
    // promise that all other input contracts have passed validation.
    if(config_.max_resident_fragments && count>config_.max_resident_fragments)
        return {InputDisposition::TooLarge,count,input_capacity()};
    auto prepared=prepare(input); // all ordinary contract errors precede mutation
    if(config_.max_resident_fragments && count>config_.max_resident_fragments-work_.size())
        return {InputDisposition::Backpressured,count,input_capacity()};
    register_input(std::move(prepared));
    if(initial) {incremental_=true;loaded_=true;start();}
    return {InputDisposition::Registered,count,input_capacity()};
}
void HostDriver::reclaim() {
    if(!incremental_)return; // preserve finite-workload baseline and diagnostics
    const auto before=work_.size();
    std::set<SendUnitId> retired;
    for(auto it=units_.begin();it!=units_.end();) {
        const auto& u=it->second;
        bool safe=u.stage==UnitStage::Done||u.stage==UnitStage::Cancelled;
        if(u.content) {
            const auto& v=versions_.at(*u.content);
            const auto retry=v.retries.find(u.fragments.front().logical.page);
            if(retry!=v.retries.end()&&retry->second.active)safe=false;
            if(const auto* task=recovery_.active(*u.content)) {
                if(!u.replay)safe=false;
                else if(task->phase==RecoveryPhase::Replaying && u.pass==task->pass &&
                        u.fragments.front().logical.page==task->page)safe=false;
            }
        }
        if(!u.replay)for(const auto& f:u.fragments)
            if(work_.at(f.id).result==LogicalResult::Pending)safe=false;
        if(!safe) {++it;continue;}
        if(!u.replay)for(const auto& f:u.fragments)work_.erase(f.id);
        retired.insert(u.id);it=units_.erase(it);
    }
    // Remove all indices before later events can dereference retired IDs.
    for(auto& v:versions_) {
        auto& ids=v.second.normal;
        ids.erase(std::remove_if(ids.begin(),ids.end(),[&](auto id){return retired.count(id);}),ids.end());
    }
    for(auto it=initial_enqueues_.begin();it!=initial_enqueues_.end();) {
        auto& ids=it->second;
        ids.erase(std::remove_if(ids.begin(),ids.end(),[&](const auto& x){return retired.count(x.second);}),ids.end());
        if(ids.empty())it=initial_enqueues_.erase(it);else ++it;
    }
    std::set<GlobalRequestId> referenced;
    for(const auto& u:units_)if(!u.second.replay)
        for(const auto& f:u.second.fragments)referenced.insert(f.parent);
    for(auto it=requests_.begin();it!=requests_.end();)
        if(results_.at(it->first).state!=LogicalResult::Pending && !referenced.count(it->first))it=requests_.erase(it);
        else ++it;
    for(auto it=release_globals_.begin();it!=release_globals_.end();) {
        auto& ids=it->second;
        ids.erase(std::remove_if(ids.begin(),ids.end(),[&](auto id){return !requests_.count(id);}),ids.end());
        if(ids.empty()) {
            const auto event=release_events_.at(it->first);
            events_.cancel(event);owned_events_.erase(event);release_events_.erase(it->first);
            it=release_globals_.erase(it);
        } else ++it;
    }
    if(work_.size()<before) {
        input_generation_=checked_add(input_generation_,1);
        if(input_capacity_handler_&&!input_capacity_event_)
            input_capacity_event_=schedule(events_.now(),[this]{input_capacity_event_.reset();input_capacity_handler_(input_capacity());});
    }
}
} // namespace hbf_eventlab
