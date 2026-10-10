#include "hbf_eventlab/host/host_driver.hpp"
#include <algorithm>

namespace hbf_eventlab {
bool HostDriver::retry_eligible(const Unit& u) const {
    const auto& retries=versions_.at(*u.content).retries;
    const auto page=u.fragments.front().logical.page;
    for(const auto& entry:retries) {
        const auto& r=entry.second;
        if(!r.active || r.pass!=u.pass)continue;
        if(page>entry.first)return false;
        if(page==entry.first && r.phase!=RetryPhase::Repairing)return false;
    }
    return true;
}

bool HostDriver::page_succeeded(ContentKey key,PageId page,std::uint64_t pass) const {
    bool found=false;
    for(const auto& item:units_) {
        const auto& u=item.second;
        if(!u.content || *u.content<key || key<*u.content || u.pass!=pass ||
           u.fragments.front().logical.page!=page)continue;
        found=true;
        if(u.replay) {if(u.stage!=UnitStage::Done || !u.successful)return false;}
        else for(const auto& f:u.fragments)if(work_.at(f.id).result!=LogicalResult::Success)return false;
    }
    return found;
}

void HostDriver::requeue(Unit& u) {
    if(u.stage!=UnitStage::Done)throw std::logic_error("retry before final response");
    u.stage=UnitStage::Queued;u.accepted=false;u.successful=false;u.command.reset();
    for(const auto& f:u.fragments)if(!u.replay)work_.at(f.id).accepted=false;
    channels_[u.fragments.front().address.channel].queue.insert(u.id);
}

void HostDriver::note_write_retry(Unit& u,const WriteResponse& response) {
    const auto key=*u.content;const auto page=u.fragments.front().logical.page;
    auto& version=versions_.at(key);auto& r=version.retries[page];
    if(r.pass!=u.pass)r=PageRetry{};
    r.pass=u.pass;
    // Only the failed command's tentative progress is invalidated. Other fragments
    // may already be resident, even when this error response was delayed.
    if(!u.replay)for(const auto& f:u.fragments)version.accepted[page].reset(f.logical.byte_offset/64);
    if(response.status==WriteStatus::PendingDluLimit) {
        u.retry=RetryKind::PendingLimit;
        u.retry_at=Tick{checked_add(events_.now().value,config_.pending_retry_delay.value)};
        if(!config_.max_pending_retries) {abort_version(key,"WriteStatus:4");return;}
        if(!r.active)r.phase=RetryPhase::Repairing;
    } else if(response.status==WriteStatus::AssemblyTimeout) {
        u.retry=RetryKind::Timeout;
        if(!config_.max_timeout_retries) {abort_version(key,"WriteStatus:5");return;}
        r.phase=RetryPhase::Draining;
    } else {
        u.retry=RetryKind::Order;
        if(!config_.max_pending_retries && !config_.max_timeout_retries) {
            abort_version(key,"WriteStatus:6");return;
        }
        if(!r.active)r.phase=RetryPhase::AwaitingPredecessor;
        r.order_explained=r.order_explained||u.order_retry_allowed;
    }
    r.active=true;
    if(response.status==WriteStatus::PendingDluLimit || response.status==WriteStatus::AssemblyTimeout) {
        // Link/Ingress followers were already issued using tentative progress.
        // Mark these concrete attempts, not every future order error in this block.
        for(auto& item:units_) {
            auto& later=item.second;
            if(!later.content || *later.content<key || key<*later.content || later.pass!=u.pass ||
               later.fragments.front().logical.page<=page)continue;
            if(later.stage==UnitStage::Handed || (later.stage==UnitStage::Done && later.retry==RetryKind::Order)) {
                later.order_retry_allowed=true;
                auto it=version.retries.find(later.fragments.front().logical.page);
                if(it!=version.retries.end() && it->second.active)it->second.order_explained=true;
            }
        }
    }
    emit(response.status==WriteStatus::PendingDluLimit?"pending-limit":
         response.status==WriteStatus::AssemblyTimeout?"assembly-timeout":"order-wait",u,*u.command);
}

bool HostDriver::advance_write_retries() {
    bool changed=false;
    for(auto& entry:versions_) {
        const auto key=entry.first;auto& version=entry.second;
        if(version.aborted)continue;
        const auto* recovery=recovery_.active(key);
        if(recovery && recovery->phase==RecoveryPhase::Draining)continue;
        // The earliest affected page is repaired first. Its valid pending fragments
        // must keep progressing: draining an entire block here would deadlock them.
        auto it=std::find_if(version.retries.begin(),version.retries.end(),[](const auto& x){return x.second.active;});
        if(it==version.retries.end())continue;
        const auto page=it->first;auto& r=it->second;
        std::vector<SendUnitId> page_units;
        bool handed=false,prior_handed=false;
        for(const auto& item:units_) {
            const auto& u=item.second;
            if(!u.content || *u.content<key || key<*u.content || u.pass!=r.pass)continue;
            const auto p=u.fragments.front().logical.page;
            if(p<page && u.stage==UnitStage::Handed)prior_handed=true;
            if(p==page) {page_units.push_back(u.id);handed|=u.stage==UnitStage::Handed;}
        }
        if(r.phase==RetryPhase::Draining) {
            // This page cannot have completed its assembly; late old fragments may
            // create another context and timeout too. Keep tracking until all settle.
            if(handed)continue;
            if(r.timeout_rounds>=config_.max_timeout_retries) {
                abort_version(key,"AssemblyRetryBudgetExhausted");changed=true;continue;
            }
            std::bitset<fragments_per_dlu> coverage;bool available=true,missing=false;
            for(auto id:page_units) {
                const auto& u=units_.at(id);
                for(const auto& f:u.fragments) {
                    coverage.set(f.logical.byte_offset/64);
                    if(!u.replay && events_.now()<requests_.at(f.parent).not_before)available=false;
                    if(u.handoffs && !retention_.find(key,page,f.logical.byte_offset/64))missing=true;
                }
            }
            if(!coverage.all() || missing) {
                abort_version(key,"WriteStatus:5 MissingRetryData");changed=true;continue;
            }
            if(!available)continue; // existing workload release event will wake us
            ++r.timeout_rounds;++stats_.timeout_retry_rounds;
            version.accepted[page].reset();
            for(auto id:page_units) {
                auto& u=units_.at(id);u.assembly_round=r.timeout_rounds;u.retry=RetryKind::Timeout;
                if(u.stage==UnitStage::Done)requeue(u);
                else if(u.stage!=UnitStage::Queued)throw std::logic_error("invalid timeout retry lifecycle");
            }
            r.phase=RetryPhase::Repairing;changed=true;
        }
        if(r.phase==RetryPhase::AwaitingPredecessor) {
            if(!r.order_explained) {
                if(prior_handed)continue;
                abort_version(key,"UnexplainedWriteOrderViolation");changed=true;continue;
            }
            // Conservative repair-only boundary. Normal writes retain acceptance
            // pipelining; after an error wait for the predecessor's real success.
            if(page && !page_succeeded(key,page-1,r.pass))continue;
            r.phase=RetryPhase::Repairing;changed=true;
        }
        if(page_succeeded(key,page,r.pass)) {r.active=false;changed=true;continue;}
        for(auto id:page_units) {
            auto& u=units_.at(id);
            if(u.stage!=UnitStage::Done || u.successful)continue;
            if(u.retry==RetryKind::PendingLimit) {
                if(u.pending_retries>=config_.max_pending_retries) {
                    abort_version(key,"PendingRetryBudgetExhausted");changed=true;break;
                }
                if(capacity_progress_[key.block.channel]<=u.capacity_at_handoff && events_.now()<u.retry_at)continue;
                requeue(u);changed=true;
            } else if(u.retry==RetryKind::Order) {
                if(!u.order_retry_allowed) {
                    if(prior_handed)continue;
                    abort_version(key,"UnexplainedWriteOrderViolation");changed=true;break;
                }
                requeue(u);changed=true;
            }
        }
    }
    return changed;
}

void HostDriver::update_retry_wakeup() {
    std::optional<Tick> next;
    for(const auto& item:units_) {
        const auto& u=item.second;
        if(!u.content || versions_.at(*u.content).aborted || u.stage!=UnitStage::Done || u.retry!=RetryKind::PendingLimit)continue;
        if(!retry_eligible(u))continue;
        const auto progress=capacity_progress_.find(u.fragments.front().address.channel);
        if(progress!=capacity_progress_.end() && progress->second>u.capacity_at_handoff)continue;
        if(events_.now()<u.retry_at && (!next || u.retry_at<*next))next=u.retry_at;
    }
    if(next==retry_wakeup_at_)return;
    if(retry_wakeup_) {events_.cancel(*retry_wakeup_);owned_events_.erase(*retry_wakeup_);}
    retry_wakeup_.reset();retry_wakeup_at_=next;
    if(next)retry_wakeup_=schedule(*next,[this] {
        retry_wakeup_.reset();retry_wakeup_at_.reset();
        for(ChannelId ch=0;ch<channels_.size();++ch)request_pump(ch);
    });
}
} // namespace hbf_eventlab
