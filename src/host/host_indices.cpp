#include "hbf_eventlab/host/host_driver.hpp"

namespace hbf_eventlab {
namespace {
template<class Map,class Key>
void member(Map& index,const Key& key,SendUnitId id,bool present) {
    if(present) {index[key].insert(id);return;}
    const auto it=index.find(key);
    if(it==index.end())return;
    it->second.erase(id);
    if(it->second.empty())index.erase(it);
}
template<class Map,class Key>
bool has_predecessor(const Map& index,const Key& key,SendUnitId id) {
    const auto it=index.find(key);
    return it!=index.end() && *it->second.begin()<id;
}
}
void HostDriver::update_read_indices(const Unit& u) {
    if(u.op!=Op::Read)return;
    const auto& f=u.fragments.front();
    if(u.group)member(queued_group_reads_,std::make_pair(f.address.channel,*u.group),u.id,u.stage==UnitStage::Queued);
    // AllRegular cannot create a Batch tail. TailBatch tracks every read, including
    // those created before register_input marks the last Unit as Batch.
    if(config_.hints==ReadHintMode::TailBatch)
        member(unaccepted_reads_,std::make_pair(f.address.channel,f.parent),u.id,
               !u.accepted && u.stage!=UnitStage::Done && u.stage!=UnitStage::Cancelled);
}
bool HostDriver::read_predecessor_pending(const Unit& u) const {
    const auto& f=u.fragments.front();
    return (u.group && has_predecessor(queued_group_reads_,std::make_pair(f.address.channel,*u.group),u.id)) ||
        (u.hint==ReadHint::Batch && has_predecessor(unaccepted_reads_,std::make_pair(f.address.channel,f.parent),u.id));
}
void HostDriver::update_pending_retry_index(const Unit& u) {
    if(u.content && !versions_.at(*u.content).aborted && u.stage==UnitStage::Done && u.retry==RetryKind::PendingLimit)
        pending_limit_retry_units_.insert(u.id);
    else pending_limit_retry_units_.erase(u.id);
}
void HostDriver::update_global_reclaimability(GlobalRequestId id) {
    if(global_fragment_refs_.at(id)==0 && results_.at(id).state!=LogicalResult::Pending)
        reclaimable_globals_.insert(id);
    else reclaimable_globals_.erase(id);
}
void HostDriver::index_unit(const Unit& u) {
    if(!u.replay)for(const auto& f:u.fragments)++global_fragment_refs_.at(f.parent);
    update_read_indices(u);
}
void HostDriver::unindex_unit(const Unit& u) {
    pending_limit_retry_units_.erase(u.id);
    if(u.op==Op::Read) {
        const auto& f=u.fragments.front();
        if(u.group)member(queued_group_reads_,std::make_pair(f.address.channel,*u.group),u.id,false);
        member(unaccepted_reads_,std::make_pair(f.address.channel,f.parent),u.id,false);
    }
    if(!u.replay)for(const auto& f:u.fragments) {
        auto& refs=global_fragment_refs_.at(f.parent);
        if(!refs)throw std::logic_error("Global resident reference underflow");
        --refs;update_global_reclaimability(f.parent);
    }
}
} // namespace hbf_eventlab
