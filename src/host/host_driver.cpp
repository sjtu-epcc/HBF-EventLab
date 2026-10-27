#include "hbf_eventlab/host/host_driver.hpp"
#include <algorithm>
namespace hbf_eventlab {
void HostDriver::set_runtime_profile(RuntimeProfile& profile) {
    if(loaded_||started_||profile_)throw std::logic_error("attach runtime profile once before Host input/start");
    profile_=&profile;splitter_.set_runtime_profile(profile_);
}
void HostDriver::set_statistics_sink(IStatisticsSink& sink) {
    if(loaded_||started_||statistics_sink_)throw std::logic_error("attach Host statistics once before load");
    statistics_sink_=&sink;
}
HostDriver::HostDriver(IHostPort& p,EventQueue& e,const IHostInterleaver& i,HostConfiguration c,std::unique_ptr<IHostSendPolicy> policy)
    :port_(p),events_(e),splitter_(i,p.layout()),config_(c),policy_(std::move(policy)),
     retention_(c.retained_fragments),recovery_(c.max_replay_passes),channels_(p.description().geometry().host_channels) {
    c.validate();
    if(&p.notifications().events()!=&e) throw std::invalid_argument("Host and Device must share event clock");
    if(policy_) config_.send_policy="custom";
    else policy_=make_host_send_policy(c.send_policy);
    if(c.writes==WriteCommandMode::Burst4096&&!p.description().capabilities().write_burst)
        throw std::invalid_argument("4KiB write burst unsupported by profile");
}
HostDriver::~HostDriver() {
    *alive_=false;
    for(auto id:owned_events_)events_.cancel(id);
    for(auto& c:channels_)if(c.wait)port_.notifications().cancel(*c.wait);
}
SendUnitId HostDriver::add_unit(Op op,std::vector<Fragment> fragments,std::optional<ContentKey> content,
                               std::optional<ReadGroup> group,bool replay,std::uint64_t pass) {
    if(fragments.empty())throw std::logic_error("empty send unit");
    next_unit_=checked_add(next_unit_,1);const SendUnitId id{next_unit_};const auto ch=fragments.front().address.channel;
    Unit u{id,op,std::move(fragments),content,group};u.replay=replay;u.pass=pass;
    auto arrival=std::make_pair(Tick{},std::uint64_t{});
    if(!replay)for(const auto& f:u.fragments)
        arrival=std::max(arrival,std::make_pair(requests_.at(f.parent).not_before,f.id.value));
    u.queued_at=std::max(events_.now(),arrival.first);
    units_.emplace(id,std::move(u));channels_[ch].queue.insert(id);
    if(replay)enqueue(units_.at(id),events_.now());
    else initial_enqueues_[arrival.first].push_back({arrival.second,id});
    if(content&&!replay)versions_[*content].normal.push_back(id);
    return id;
}
void HostDriver::enqueue(Unit& u,Tick at) {
    if(u.stage!=UnitStage::Queued)throw std::logic_error("enqueue of nonqueued Host unit");
    next_enqueue_=checked_add(next_enqueue_,1);
    u.enqueue_sequence=next_enqueue_;u.queued_at=at;
    channels_[u.fragments.front().address.channel].queue.insert(u.id);
}
void HostDriver::release_units(Tick now) {
    while(!initial_enqueues_.empty() && !(now<initial_enqueues_.begin()->first)) {
        auto it=initial_enqueues_.begin();
        for(const auto& item:it->second) {
            auto& u=units_.at(item.second);
            if(u.stage==UnitStage::Queued && !u.enqueue_sequence)enqueue(u,u.queued_at);
        }
        initial_enqueues_.erase(it);
    }
}
EventId HostDriver::schedule(Tick at,std::function<void()> f) {
    auto id=std::make_shared<EventId>();
    *id=events_.schedule_at(at,EventPhase::Arrival,[this,id,f=std::move(f)]{owned_events_.erase(*id);f();reclaim();});
    owned_events_.insert(*id);return *id;
}
void HostDriver::request_pump(ChannelId ch) {
    if(channels_[ch].pump)return;
    channels_[ch].pump=schedule(events_.now(),[this,ch]{channels_[ch].pump.reset();pump(ch);});
}
bool HostDriver::dependencies_ready(GlobalRequestId id) const {
    for(auto dep:requests_.at(id).after)if(results_.at(dep).state!=LogicalResult::Success)return false;
    return true;
}
bool HostDriver::version_done(ContentKey key) const {
    if(versions_.at(key).unfinished)return false;
    const auto it=recovery_.tasks().find(key);
    return it==recovery_.tasks().end() || it->second.phase==RecoveryPhase::Completed || it->second.phase==RecoveryPhase::Failed;
}
bool HostDriver::eligible(const Unit& u) const {
    if(u.stage!=UnitStage::Queued || !u.enqueue_sequence)return false;
    const auto& f=u.fragments.front();const auto ch=f.address.channel;
    if(!u.replay)for(const auto& fragment:u.fragments) {
        const auto& request=requests_.at(fragment.parent);
        if(events_.now()<request.not_before||!dependencies_ready(fragment.parent))return false;
    }
    if(u.op==Op::Read) {
        for(const auto& item:units_) {
            const auto& prior=item.second;
            if(prior.id.value>=u.id.value)break;
            if(prior.op!=Op::Read || prior.fragments.front().address.channel!=ch)continue;
            if(u.group && prior.group==u.group && prior.stage==UnitStage::Queued)return false;
            if(u.hint==ReadHint::Batch && prior.fragments.front().parent==f.parent &&
               !prior.accepted && prior.stage!=UnitStage::Done && prior.stage!=UnitStage::Cancelled)return false;
        }
        return true;
    }
    const auto key=*u.content;const auto& version=versions_.at(key);
    if(version.aborted)return false;
    if(!retry_eligible(u))return false;
    if(u.replay) {
        const auto& t=recovery_.tasks().at(key);
        if(t.phase!=RecoveryPhase::Replaying||t.pass!=u.pass||t.page!=f.logical.page)return false;
    } else {
        const auto r=recovery_.tasks().find(key);
        if(r!=recovery_.tasks().end() && (r->second.phase==RecoveryPhase::Draining||r->second.phase==RecoveryPhase::Replaying))return false;
        for(const auto& older:version_order_.at(key.block)) {
            if(older.content==key.content)break;
            if(!version_done(older)||versions_.at(older).aborted)return false;
        }
        if(f.logical.page) {
            const auto prev=version.accepted.find(f.logical.page-1);
            if(prev==version.accepted.end()||!prev->second.all())return false;
        }
    }
    const PageKey page{key,f.logical.page};
    return channels_[ch].open.count(page)||channels_[ch].open.size()<config_.max_open_dlus_per_channel;
}
void HostDriver::emit(const char* action,const Unit& u,CommandId command) {
    if(trace_)trace_({action,events_.now(),command,u.id,u.fragments.front().logical.block,u.pass});
}
void HostDriver::pump(ChannelId ch) {
    RuntimeScope timing(profile_,RuntimeRegion::Pump);
    auto& channel=channels_[ch];
    if(channel.wait) {port_.notifications().cancel(*channel.wait);channel.wait.reset();}
    advance();
    std::vector<WaitKey> waits;
    std::set<SendUnitId> tried;
    for(;;) {
        std::vector<HostSendCandidate> candidates;
        for(auto uid:channel.queue) {
            if(profile_)profile_->add(RuntimeCounter::PumpQueueVisits);
            const auto& u=units_.at(uid);if(tried.count(uid)||!eligible(u))continue;
            const bool open=u.content && channel.open.count({*u.content,u.fragments.front().logical.page});
            candidates.push_back({uid,open,u.replay,u.enqueue_sequence});
        }
        if(candidates.empty())break;
        if(profile_)profile_->add(RuntimeCounter::PumpCandidates,candidates.size());
        const auto chosen=policy_->select(candidates);
        if(std::none_of(candidates.begin(),candidates.end(),[&](const auto& c){return c.id==chosen;}))
            throw std::logic_error("Host policy selected a noncandidate");
        auto& u=units_.at(chosen);const auto& f=u.fragments.front();
        if(!u.command)u.command=CommandId{checked_add(next_command_,1)},next_command_=u.command->value;
        const auto command=*u.command;
        LocalTransaction tx{{command,u.op,f.address,static_cast<std::uint32_t>(u.fragments.size()*64),0,u.hint,u.group},{}};
        if(u.op==Op::Write)for(const auto& fragment:u.fragments)tx.data.push_back(fragment.data);
        const auto life=std::weak_ptr<bool>(alive_);
        if(profile_)profile_->add(RuntimeCounter::HandoffAttempts);
        const auto handoff=port_.try_submit(tx,[this,life,command](const auto& response) {
            if(auto live=life.lock();live&&*live)receive(command,response);
        },[this,life,command](const auto& notice) {
            if(auto live=life.lock();live&&*live)accepted(command,notice);
        });
        if(handoff.disposition==HandoffDisposition::NotTaken) {
            tried.insert(chosen);waits.insert(waits.end(),handoff.waits.begin(),handoff.waits.end());
            if(std::any_of(handoff.waits.begin(),handoff.waits.end(),[](auto k){return k.reason==WaitReason::RequestLink;}))break;
            continue;
        }
        if(!handoff.attempt)throw std::logic_error("Taken without attempt identity");
        if(profile_)profile_->add(RuntimeCounter::HandoffTaken);
        u.capacity_at_handoff=capacity_progress_[ch];u.order_retry_allowed=false;
        if(u.retry==RetryKind::PendingLimit) {++u.pending_retries;++stats_.pending_retry_commands;}
        else if(u.retry==RetryKind::Timeout && u.handoffs)++stats_.timeout_retry_commands;
        else if(u.retry==RetryKind::Order)++stats_.order_retry_commands;
        ++u.handoffs;
        if(statistics_sink_) {
            std::vector<LogicalWorkId> work;
            if(!u.replay)for(const auto& fragment:u.fragments)work.push_back(fragment.id);
            statistics_sink_->observe(StatAttemptLinked{*handoff.attempt,u.id,u.queued_at,u.handoffs,u.replay,u.pass,
                u.content?std::optional<std::uint64_t>{u.content->content}:std::nullopt,std::move(work)});
        }
        u.stage=UnitStage::Handed;channel.queue.erase(chosen);
        attempts_.emplace(command,Attempt{chosen,*handoff.attempt});
        unsettled_[f.logical.block].insert(command);
        if(u.content) {
            ++channel.open[{*u.content,f.logical.page}];
            if(!u.replay)for(const auto& fragment:u.fragments)retention_.save(*u.content,fragment);
        }
        ++stats_.handed;if(u.replay)++stats_.replay_commands;emit("handoff",u,command);
    }
    if(!waits.empty()&&!channel.queue.empty()) {
        std::vector<WaitObservation> observations;
        for(auto key:waits)observations.push_back(port_.notifications().observe(key));
        channel.wait=port_.notifications().subscribe(observations,[this,ch]{channels_[ch].wait.reset();request_pump(ch);});
    }
}
void HostDriver::accepted(CommandId id,const AcceptedNotice& notice) {
    auto& attempt=attempts_.at(id);auto& u=units_.at(attempt.unit);
    if(attempt.transfer!=notice.attempt||id!=notice.command||u.fragments.front().address.channel!=notice.channel||attempt.accepted)
        throw std::logic_error("duplicate/mismatched acceptance notice");
    attempt.accepted=true;u.accepted=true;++stats_.accepted;
    if(u.content) {
        if(u.replay) {if(recovery_.accepted(*u.content,u.pass,u.fragments.front().logical.page))++stats_.replay_passes;}
        else for(const auto& f:u.fragments)versions_.at(*u.content).accepted[f.logical.page].set(f.logical.byte_offset/64);
    }
    if(!u.replay)for(const auto&f:u.fragments)work_.at(f.id).accepted=true;
    emit("accepted",u,id);request_pump(notice.channel);
}
void HostDriver::finish_work(LogicalWorkId id,bool success,const std::string& reason) {
    auto& work=work_.at(id);if(work.result!=LogicalResult::Pending)return; // historical success never rewritten
    work.result=success?LogicalResult::Success:LogicalResult::Failed;work.needs_recovery=false;
    if(requests_.at(work.fragment.parent).op==Op::Write) {
        auto& version=versions_.at({work.fragment.logical.block,requests_.at(work.fragment.parent).content});
        if(!version.unfinished)throw std::logic_error("version completion underflow");
        --version.unfinished;
        if(success)version.successful[work.fragment.logical.page].set(work.fragment.logical.byte_offset/host_unit_bytes);
    }
    auto& result=results_.at(work.fragment.parent);
    if(!result.remaining)throw std::logic_error("logical work completed twice");
    --result.remaining;
    if(success)++stats_.logical_success;
    else {++stats_.logical_failed;++result.failed;if(result.reason.empty())result.reason=reason;}
    if(!result.remaining)result.state=result.failed?LogicalResult::Failed:LogicalResult::Success;
    if(statistics_sink_) {
        statistics_sink_->observe(StatWorkFinished{id,events_.now(),success,success?std::string{}:reason});
        if(!result.remaining)statistics_sink_->observe(StatGlobalFinished{result.id,events_.now(),
            result.state==LogicalResult::Success,result.reason});
    }
    if(!result.remaining && completion_) {
        const GlobalCompletion completed{result,events_.now()};
        // Run after receive/advance finishes updating attempts and retry state.
        schedule(events_.now(),[this,completed]{completion_(completed);});
    }
}
void HostDriver::cancel_unit(Unit& u,const std::string& reason) {
    if(u.stage!=UnitStage::Queued)return;
    channels_[u.fragments.front().address.channel].queue.erase(u.id);u.stage=UnitStage::Cancelled;
    if(!u.replay)for(const auto& f:u.fragments)finish_work(f.id,false,reason);
}
void HostDriver::abort_version(ContentKey key,const std::string& reason) {
    auto& v=versions_.at(key);v.aborted=true;v.reason=reason;retention_.unpin(key);
    for(auto& retry:v.retries)retry.second.active=false;
    if(recovery_.active(key))recovery_.fail(key,reason);
    for(auto& item:units_) {
        auto& u=item.second;
        if(!u.content || *u.content<key || key<*u.content)continue;
        if(u.stage==UnitStage::Queued)cancel_unit(u,reason);
        else if(!u.replay && u.stage==UnitStage::Done)for(const auto& f:u.fragments)finish_work(f.id,false,reason);
    }
}
void HostDriver::receive(CommandId id,const DeliveredResponse& response) {
    RuntimeScope timing(profile_,RuntimeRegion::Receive);
    auto it=attempts_.find(id);
    if(it==attempts_.end()||it->second.transfer!=response.attempt)throw std::logic_error("duplicate/mismatched Host response");
    auto& u=units_.at(it->second.unit);const auto block=u.fragments.front().logical.block;
    if(!it->second.accepted)throw std::logic_error("response before acceptance notice");
    bool success=false;
    if(u.op==Op::Write) {
        const auto& wr=std::get<WriteResponse>(response.response);
        if(wr.command!=id||wr.channel!=block.channel)throw std::logic_error("response identity mismatch");
        success=wr.status==WriteStatus::Success;
        stats_.pending_limit_errors+=wr.status==WriteStatus::PendingDluLimit;
        stats_.assembly_timeout_errors+=wr.status==WriteStatus::AssemblyTimeout;
        if(wr.status==WriteStatus::Success || wr.status==WriteStatus::AssemblyTimeout || wr.status==WriteStatus::ProgramFail) {
            // One DLU release, not 64 capacity wakeups for its command responses.
            if(capacity_completions_.emplace(*u.content,u.fragments.front().logical.page,u.pass,u.assembly_round).second)
                ++capacity_progress_[block.channel];
        }
        const bool recoverable=wr.status==WriteStatus::ProgramFail||(wr.status==WriteStatus::WriteOrderViolation&&wr.additional_info==2);
        if(!success&&!versions_.at(*u.content).aborted&&recoverable) {
            const auto root=wr.status==WriteStatus::ProgramFail?std::optional<PageId>{u.fragments.front().logical.page}:std::nullopt;
            auto& task=recovery_.notice(*u.content,u.pass,root,unsettled_.at(block));
            (void)task;stats_.recovery_tasks=recovery_.created();
            for(auto& retry:versions_.at(*u.content).retries)retry.second.active=false;
            // Frozen old set includes Link/Ingress/accepted attempts. Later reads are
            // not appended. Unsent normal writes remain logical work, not completions.
            for(auto& item:units_) {
                auto& old=item.second;
                if(old.replay && old.content && !(*old.content<*u.content) && !(*u.content<*old.content) &&
                   old.pass==u.pass && old.stage==UnitStage::Queued)cancel_unit(old,"RestartReplay");
            }
            if(!u.replay)for(const auto& f:u.fragments)work_.at(f.id).needs_recovery=true;
            emit(root?"program-fail":"follower-fail",u,id);
        } else if(!success) {
            const auto* recovery=recovery_.active(*u.content);
            const bool resource_error=wr.status==WriteStatus::PendingDluLimit || wr.status==WriteStatus::AssemblyTimeout ||
                (wr.status==WriteStatus::WriteOrderViolation && wr.additional_info==1);
            if(resource_error && !versions_.at(*u.content).aborted) {
                // An existing block replay supersedes resource repair of old attempts.
                if(!(recovery && recovery->phase==RecoveryPhase::Draining))note_write_retry(u,wr);
            } else abort_version(*u.content,"WriteStatus:"+std::to_string(static_cast<unsigned>(wr.status)));
        }
        if(success)u.retry=RetryKind::None;
        if(!u.replay)for(const auto& f:u.fragments)
            if(success)finish_work(f.id,true);
            else if(versions_.at(*u.content).aborted)finish_work(f.id,false,versions_.at(*u.content).reason);
    } else {
        const auto& rr=std::get<ReadResponse>(response.response);
        if(rr.command!=id||rr.channel!=block.channel)throw std::logic_error("response identity mismatch");
        success=rr.status==ReadStatus::Success;
        const auto& request=requests_.at(u.fragments.front().parent);
        if(success&&request.expected_content)success=rr.data==token_for(*request.expected_content,u.fragments.front().global);
        finish_work(u.fragments.front().id,success,rr.status==ReadStatus::Success?"DataMismatch":
                    "ReadStatus:"+std::to_string(static_cast<unsigned>(rr.status)));
    }
    if(statistics_sink_)statistics_sink_->observe(StatAttemptAssessed{response.attempt,events_.now(),success});
    ++stats_.responses;if(!success)++stats_.attempt_errors;
    u.stage=UnitStage::Done;u.successful=success;
    if(u.content) {
        auto& open=channels_[block.channel].open;const PageKey page{*u.content,u.fragments.front().logical.page};
        auto entry=open.find(page);if(entry==open.end()||!entry->second)throw std::logic_error("Host DLU accounting underflow");
        if(--entry->second==0)open.erase(entry);
    }
    attempts_.erase(it);unsettled_.at(block).erase(id);recovery_.settled(block,id);
    emit("response",u,id);advance();
    for(ChannelId ch=0;ch<channels_.size();++ch)request_pump(ch);
    reclaim(); // all references used by this response/advance are now finished
}
void HostDriver::make_replay(ContentKey key,RecoveryTask& t) {
    std::vector<Fragment> page;
    for(std::uint32_t slot=0;slot<64;++slot) {
        const auto saved=retention_.find(key,t.page,slot);
        if(!saved)throw std::logic_error("verified replay data disappeared");
        auto fragment=splitter_.fragment(saved->global,key.content);
        if(!(fragment.logical.block==key.block)||fragment.logical.page!=t.page||fragment.logical.byte_offset!=slot*64)
            throw std::logic_error("retained Global address no longer maps to replay prefix");
        fragment.data=saved->token;page.push_back(fragment);
    }
    if(config_.writes==WriteCommandMode::Burst4096)add_unit(Op::Write,std::move(page),key,{},true,t.pass);
    else for(auto& f:page)add_unit(Op::Write,{f},key,{},true,t.pass);
}
bool HostDriver::advance_recovery(ContentKey key,RecoveryTask& t) {
    if(t.phase==RecoveryPhase::Draining) {
        if(!t.drain.empty()||!t.through)return false;
        if(!retention_.pin_prefix(key,*t.through)) {abort_version(key,"MissingRecoveryData");return true;}
        auto& v=versions_.at(key);
        if(!recovery_.begin_pass(key,++v.next_pass)) {abort_version(key,"RecoveryBudgetExhausted");return true;}
        make_replay(key,t);return true;
    }
    if(t.phase!=RecoveryPhase::Replaying)return false;
    bool found=false;
    for(const auto& item:units_) {
        const auto& u=item.second;
        if(!u.replay || !u.content || *u.content<key || key<*u.content || u.pass!=t.pass || u.fragments.front().logical.page!=t.page)continue;
        found=true;if(u.stage!=UnitStage::Done||!u.successful)return false;
    }
    if(!found)return false;
    if(t.page<*t.through) {++t.page;make_replay(key,t);return true;}
    const auto through=*t.through;
    recovery_.complete(key);retention_.unpin(key);
    auto& version=versions_.at(key);
    for(PageId p=0;p<=through;++p)version.accepted[p].set();
    for(auto uid:version.normal) {
        auto& u=units_.at(uid);
        if(u.fragments.front().logical.page<=through) {
            for(const auto& f:u.fragments) {
                if(work_.at(f.id).result==LogicalResult::Pending) {finish_work(f.id,true);++stats_.recovered_fragments;}
                version.accepted[f.logical.page].set(f.logical.byte_offset/64);
            }
            if(u.stage==UnitStage::Queued) {channels_[key.block.channel].queue.erase(uid);u.stage=UnitStage::Done;}
        } else if(u.stage==UnitStage::Done&&!u.successful) {
            requeue(u);
            for(const auto& f:u.fragments) {work_.at(f.id).needs_recovery=false;work_.at(f.id).accepted=false;}
            version.accepted[u.fragments.front().logical.page].reset();
        }
    }
    return true;
}
void HostDriver::advance() {
    RuntimeScope timing(profile_,RuntimeRegion::Advance);
    // Finite metadata closure. Never use zero-Tick self-polling while hardware waits.
    bool changed;
    do {
        changed=false;
        for(auto& item:versions_)if(!item.second.aborted) {
            for(const auto& older:version_order_.at(item.first.block)) {
                if(older.content==item.first.content)break;
                if(versions_.at(older).aborted) {abort_version(item.first,"PriorContentFailed");changed=true;break;}
            }
        }
        for(auto& item:versions_)if(auto* task=recovery_.active(item.first))
            changed=advance_recovery(item.first,*task)||changed;
        changed=advance_write_retries()||changed;
        for(auto& item:units_) {
            auto& u=item.second;if(u.op!=Op::Read||u.stage!=UnitStage::Queued)continue;
            for(auto dep:requests_.at(u.fragments.front().parent).after)
                if(results_.at(dep).state==LogicalResult::Failed) {cancel_unit(u,"DependencyFailed");changed=true;break;}
        }
    } while(changed);
    // A canceled dependent request has no future release obligation. Do not let
    // an obsolete far-future arrival turn a fully settled workload into LimitReached.
    bool settled;
    if(profile_) {
        RuntimeScope scan(profile_,RuntimeRegion::ResultsScan);
        std::uint64_t visited=0;
        settled=std::all_of(results_.begin(),results_.end(),[&](const auto& r){++visited;return r.second.state!=LogicalResult::Pending;});
        profile_->add(RuntimeCounter::ResultsEntriesVisited,visited);
        profile_->peak(RuntimeCounter::ResultsSizePeak,results_.size());
    } else {
        settled=std::all_of(results_.begin(),results_.end(),[](const auto& r){return r.second.state!=LogicalResult::Pending;});
    }
    if(settled) {
        for(const auto& event:release_events_) {events_.cancel(event.second);owned_events_.erase(event.second);}
        release_events_.clear();
        release_globals_.clear();
        initial_enqueues_.clear();
    }
    // Cancel passive source waits if no sendable work remains on that channel.
    for(auto& ch:channels_)if(ch.queue.empty()&&ch.wait) {port_.notifications().cancel(*ch.wait);ch.wait.reset();}
    update_retry_wakeup();
}
HostActivity HostDriver::activity() const {
    HostActivity a;
    for(const auto& r:results_)a.unfinished_global+=r.second.state==LogicalResult::Pending;
    for(const auto& c:channels_) {a.queued_units+=c.queue.size();a.subscriptions+=c.wait.has_value();}
    a.unsettled_attempts=attempts_.size();a.recoveries=recovery_.active_count();
    for(const auto& v:versions_)for(const auto& p:v.second.retries)a.recoveries+=p.second.active;
    a.events=owned_events_.size();return a;
}
bool HostDriver::workload_success() const {
    for(const auto& r:results_)if(r.second.state!=LogicalResult::Success)return false;return true;
}
std::string HostDriver::waiting_reason() const {
    if(!started_)return "HostNotStarted";
    for(const auto& v:versions_)for(const auto& p:v.second.retries)if(p.second.active)
        return p.second.phase==RetryPhase::Draining?"WaitingForTimedOutPageAttempts":
            p.second.phase==RetryPhase::AwaitingPredecessor?"WaitingForWriteOrderCause":"WaitingForPageRepair";
    for(const auto& item:recovery_.tasks()) {
        const auto& t=item.second;
        if(t.phase==RecoveryPhase::Draining&&!t.through)return "WaitingForProgramFailRoot";
        if(t.phase==RecoveryPhase::Draining&&!t.drain.empty())return "WaitingForOldAttemptResponses";
    }
    return "PendingDependenciesOrSendEligibility";
}
} // namespace hbf_eventlab

