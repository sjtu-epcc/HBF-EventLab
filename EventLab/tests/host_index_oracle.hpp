#pragma once
#include "hbf_eventlab/host/host_driver.hpp"

namespace hbf_eventlab {
// Test-only independent full scans. Never linked into the production Host.
struct HostIndexTestAccess {
    static void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
    static void validate(const HostDriver& h) {
        decltype(h.queued_group_reads_) groups;
        decltype(h.unaccepted_reads_) unaccepted;
        decltype(h.global_fragment_refs_) refs;
        decltype(h.pending_limit_retry_units_) retries;
        decltype(h.reclaimable_globals_) reclaimable;
        std::map<ContentKey,std::vector<SendUnitId>> normal;
        for(const auto& v:h.versions_)normal.emplace(v.first,std::vector<SendUnitId>{});
        for(const auto& r:h.requests_)refs.emplace(r.first,0);
        for(const auto& entry:h.units_) {
            const auto& u=entry.second;const auto& f=u.fragments.front();
            if(u.content&&!u.replay)normal.at(*u.content).push_back(u.id);
            if(!u.replay)for(const auto& fragment:u.fragments)++refs.at(fragment.parent);
            if(u.op==Op::Read) {
                if(u.group && u.stage==HostDriver::UnitStage::Queued)
                    groups[{f.address.channel,*u.group}].insert(u.id);
                if(h.config_.hints==ReadHintMode::TailBatch && !u.accepted &&
                   u.stage!=HostDriver::UnitStage::Done && u.stage!=HostDriver::UnitStage::Cancelled)
                    unaccepted[{f.address.channel,f.parent}].insert(u.id);
                bool blocked=false;
                for(const auto& prior:h.units_) {
                    const auto& p=prior.second;
                    if(p.id.value>=u.id.value)break;
                    if(p.op!=Op::Read || p.fragments.front().address.channel!=f.address.channel)continue;
                    if(u.group && p.group==u.group && p.stage==HostDriver::UnitStage::Queued)blocked=true;
                    if(u.hint==ReadHint::Batch && p.fragments.front().parent==f.parent && !p.accepted &&
                       p.stage!=HostDriver::UnitStage::Done && p.stage!=HostDriver::UnitStage::Cancelled)blocked=true;
                }
                check(h.read_predecessor_pending(u)==blocked,"read predecessor index differs from full scan");
            }
            if(u.content && !h.versions_.at(*u.content).aborted && u.stage==HostDriver::UnitStage::Done &&
               u.retry==HostDriver::RetryKind::PendingLimit)retries.insert(u.id);
        }
        for(const auto& entry:refs)
            if(!entry.second && h.results_.at(entry.first).state!=LogicalResult::Pending)reclaimable.insert(entry.first);
        check(groups==h.queued_group_reads_,"group index differs from full scan");
        check(unaccepted==h.unaccepted_reads_,"Batch predecessor index differs from full scan");
        check(refs==h.global_fragment_refs_,"Global reference index differs from resident fragments");
        check(retries==h.pending_limit_retry_units_,"Pending retry index differs from full scan");
        check(reclaimable==h.reclaimable_globals_,"Global reclamation candidates differ from full scan");
        for(const auto& v:h.versions_) {
            check(normal.at(v.first)==v.second.normal,"version normal directory differs from live Units");
            if(!h.write_error_seen_) {
                check(!v.second.aborted,"aborted version before first write error");
                for(const auto& r:v.second.retries)check(!r.second.active,"retry before first write error");
            }
        }
        if(!h.write_error_seen_)check(h.recovery_.tasks().empty(),"recovery history before first write error");
    }
    static bool write_error_seen(const HostDriver& h) {return h.write_error_seen_;}
    static std::size_t references(const HostDriver& h,GlobalRequestId id) {return h.global_fragment_refs_.at(id);}
    static std::size_t retry_count(const HostDriver& h) {return h.pending_limit_retry_units_.size();}
    static bool no_read_indices(const HostDriver& h) {return h.queued_group_reads_.empty()&&h.unaccepted_reads_.empty();}
};
} // namespace hbf_eventlab
