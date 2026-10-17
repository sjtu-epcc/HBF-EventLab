#include "hbf_eventlab/runtime/device_driver.hpp"
#include <algorithm>

namespace hbf_eventlab {
void DeviceDriver::set_statistics_sink(IStatisticsSink& sink) {
    if(next_attempt_||statistics_sink_)throw std::logic_error("attach Device statistics once before handoff");
    statistics_sink_=&sink;
}
namespace {
ExecutionConfiguration validated(ExecutionConfiguration c,const StaticDeviceDescription& d) { c.validate(d); return c; }
Tick after(Tick now,Tick delay) { return Tick{checked_add(now.value,delay.value)}; }
PhysicalNandAddress page_address(PhysicalBlockKey b) { return {b.channel,b.die,b.bank,b.block,0,0}; }
}
bool DeviceActivity::drained() const {
    return !(unsettled_transfers || ingress || in_flight_requests || outstanding || pending_dlus ||
        bank_operations || waiting_erases || window_reads || queued_reads || egress || in_flight_responses || feedback_events ||
        protected_cache_slots || filling_cache_slots || scheduled_driver_events || queued_wakeups);
}
DeviceDriver::DeviceDriver(StaticDeviceDescription d,ExecutionConfiguration c,EventQueue& e,
        std::unique_ptr<INandSchedulingPolicy> nand,std::unique_ptr<IUcieArbitrationPolicy> link,
        std::unique_ptr<IReadDispatchPolicy> dispatch)
    :description_(std::move(d)),config_(validated(std::move(c),description_)),events_(e),
     topology_(description_.geometry(),description_.topology()),layout_(description_.geometry()),
     zones_(description_),replacements_(description_.geometry()),media_(description_.geometry()),
     capacity_(description_,layout_,zones_,replacements_,media_,description_.capacity_policy()),signals_(events_),
     admission_(description_.geometry().host_channels,description_.configuration().mocs_selected,signals_),
     egress_(description_.geometry().host_channels,config_.buffers.egress),
     writes_(description_,layout_,capacity_,media_,events_,admission_),
     reads_(description_,layout_,capacity_,media_,admission_,writes_,egress_),
     nand_policy_(std::move(nand)),link_policy_(std::move(link)),dispatch_policy_(std::move(dispatch)) {
    if(nand_policy_) config_.nand_scheduler="custom";
    else {
        if(config_.nand_scheduler!="fcfs") throw std::invalid_argument("execution.nand_scheduler: unsupported policy");
        nand_policy_=std::make_unique<FcfsNandPolicy>();
    }
    if(link_policy_) config_.ucie_scheduler="custom";
    else {
        if(config_.ucie_scheduler!="fifo") throw std::invalid_argument("execution.ucie_scheduler: unsupported policy");
        link_policy_=std::make_unique<FifoUciePolicy>();
    }
    if(dispatch_policy_) config_.read_dispatch.policy="custom";
    else {
        if(config_.read_dispatch.policy!="window") throw std::invalid_argument("execution.read_dispatch.policy: unsupported policy");
        dispatch_policy_=std::make_unique<WindowDispatchPolicy>(config_.read_dispatch.wait,config_.read_dispatch.threshold);
    }
    for(ChannelId ch=0;ch<description_.geometry().host_channels;++ch) {
        channels_.push_back(std::make_unique<Channel>(config_.buffers.ingress,*dispatch_policy_));
        watch_responses(ch);
    }
    writes_.set_cache_invalidator([this]{ cache_.invalidate_all(); });
}
DeviceDriver::~DeviceDriver() {
    for(auto id:events_owned_) events_.cancel(id);
    for(auto& ch:channels_) if(ch->response_wait) signals_.cancel(*ch->response_wait);
    for(auto& item:transactions_) if(item.second.wait) signals_.cancel(*item.second.wait);
    // Cache leases in transaction records die before cache_. Canceled event closures
    // contain only IDs, never leases referring to a subsequently destroyed cache.
}
EventId DeviceDriver::schedule(Tick at,EventPhase phase,std::function<void()> f) {
    auto id=std::make_shared<EventId>();
    *id=events_.schedule_at(at,phase,[this,id,f=std::move(f)] {
        events_owned_.erase(*id); f();
    });
    events_owned_.insert(*id); return *id;
}
void DeviceDriver::emit(const char* action,ChannelId ch,std::uint64_t id,std::optional<PhysicalNandAddress> target) {
    if(trace_) trace_({action,events_.now(),ch,id,target});
}
WaitKey DeviceDriver::bank_wait(WaitReason reason,BankKey b) { return {reason,b.channel,b.die,b.bank,0}; }
void DeviceDriver::notify_cache(BankKey b) { signals_.notify(bank_wait(WaitReason::CacheSpace,b)); }
void DeviceDriver::notify_bank(BankKey b) { signals_.notify(bank_wait(WaitReason::Bank,b)); }
const BufferBudget& DeviceDriver::ingress(ChannelId ch) const { return channels_.at(ch)->rx; }
DeviceDriver::Transaction* DeviceDriver::find(ChannelId ch,CommandId id) {
    const auto it=identities_.find({ch,id}); return it==identities_.end()?nullptr:&transactions_.at(it->second);
}
HandoffResult DeviceDriver::try_submit(const LocalTransaction& input,Receiver receiver,AcceptanceObserver acceptance) {
    const auto& cmd=input.command; const auto ch=cmd.address.channel;
    if(ch>=channels_.size() || !receiver) throw std::invalid_argument("bad channel/receiver");
    if(identities_.count({ch,cmd.id})) throw std::logic_error("duplicate unsettled command attempt");
    if((cmd.op!=Op::Read && cmd.op!=Op::Write) || (cmd.op==Op::Read && !input.data.empty()) ||
       (cmd.op==Op::Write && input.data.size()!=cmd.size_bytes/host_unit_bytes))
        throw std::invalid_argument("invalid transaction payload shape");
    if((cmd.op==Op::Read && cmd.size_bytes!=host_unit_bytes) ||
       (cmd.op==Op::Write && cmd.size_bytes!=host_unit_bytes && !(description_.capabilities().write_burst && cmd.size_bytes==dlu_bytes)))
        throw std::invalid_argument("unsupported transaction size");
    auto& channel=*channels_[ch]; std::vector<WaitKey> waits;
    const std::size_t payload=cmd.op==Op::Write?cmd.size_bytes:0;
    if(channel.request_busy) waits.push_back(WaitKey::channel_key(WaitReason::RequestLink,ch));
    if(!channel.rx.fits(payload)) waits.push_back(WaitKey::channel_key(WaitReason::IngressSpace,ch));
    if(!waits.empty()) return {HandoffDisposition::NotTaken,{},std::move(waits)};
    const TransferAttemptId id{checked_add(next_attempt_,1)};
    const auto service=checked_add(config_.ucie.overhead.request_control_bytes,payload);
    // There is no hidden unbounded request queue: source offers only when this lane
    // can start. Request-side arbitration of multiple source records belongs to Host.
    const std::vector<TransferCandidateView> offer{{id.value,id.value,cmd.op,service}};
    if(link_policy_->select(offer)!=id.value) throw std::logic_error("link policy selected a noncandidate");
    Transaction transaction{id,input,std::move(receiver),std::move(acceptance)};
    transactions_.emplace(id.value,std::move(transaction));
    identities_.emplace(CommandKey{ch,cmd.id},id.value); // Handoff registered before ANY callback/event.
    channel.transactions.insert(id.value);
    channel.rx.reserve(payload); channel.request_busy=true; ++channel.request_flights;
    next_attempt_=id.value; ++stats_.handoffs; ++stats_.request_transfers;
    const auto serialized=after(events_.now(),config_.ucie.serialization(service));
    schedule(serialized,EventPhase::Completion,[this,ch] {
        channels_[ch]->request_busy=false;
        signals_.notify(WaitKey::channel_key(WaitReason::RequestLink,ch));
    });
    schedule(after(serialized,config_.ucie.overhead.request_fixed_latency),EventPhase::Arrival,[this,ch,id] {
        auto& t=transactions_.at(id.value); t.stage=Stage::Ingress;
        --channels_[ch]->request_flights; // RX reservation becomes resident, unchanged accounting.
        if(statistics_sink_)statistics_sink_->observe(StatAttemptAdvanced{id,AttemptMilestone::RxArrival,events_.now()});
        emit("rx-arrival",ch,id.value); request_pump(ch);
    });
    if(statistics_sink_)statistics_sink_->observe(StatAttemptStarted{id,cmd,events_.now()});
    emit("handoff",ch,id.value);
    return {HandoffDisposition::Taken,id,{}};
}
void DeviceDriver::watch_responses(ChannelId ch) {
    const auto key=WaitKey::channel_key(WaitReason::ResponseReady,ch);
    channels_[ch]->response_wait=signals_.subscribe({signals_.observe(key)},[this,ch] {
        channels_[ch]->response_wait.reset(); watch_responses(ch); request_pump(ch);
    });
}
void DeviceDriver::request_pump(ChannelId ch) {
    auto& channel=*channels_[ch]; if(channel.pump) return;
    channel.pump=schedule(events_.now(),EventPhase::Arrival,[this,ch] {
        channels_[ch]->pump.reset(); pump(ch);
    });
}
void DeviceDriver::wait(Transaction& t,const std::vector<WaitKey>& keys) {
    if(t.wait) signals_.cancel(*t.wait);
    if(keys.empty()) throw std::logic_error("blocked transaction without a wake condition");
    std::vector<WaitObservation> observations;
    // Path checks and registration are one single-thread step. No callbacks run
    // between them; subscribe still compares revisions to guard later refactors.
    for(auto key:keys) observations.push_back(signals_.observe(key));
    const auto id=t.id.value;
    t.wait=signals_.subscribe(observations,[this,id] {
        auto it=transactions_.find(id); if(it==transactions_.end()) return;
        it->second.wait.reset(); request_pump(it->second.input.command.address.channel);
    });
}
bool DeviceDriver::admit(Transaction& t) {
    const auto& cmd=t.input.command; const auto ch=cmd.address.channel;
    // An unaccepted earlier member of an ordered read group must not be bypassed.
    if(cmd.op==Op::Read && cmd.read_group) {
        for(const auto identity:channels_[ch]->transactions) {
            const auto& earlier=transactions_.at(identity); const auto& e=earlier.input.command;
            if(identity>=t.id.value) break;
            if(e.address.channel==ch && e.op==Op::Read && e.read_group==cmd.read_group &&
               (earlier.stage==Stage::Link || earlier.stage==Stage::Ingress)) {
                wait(t,{WaitKey::order(ch,*cmd.read_group)}); return false;
            }
        }
    }
    ++stats_.ingress_attempts;
    if(cmd.op==Op::Write) {
        const auto result=writes_.submit(cmd,t.input.data);
        if(result.admission==SubmitResult::Backpressured) { wait(t,result.waits); return false; }
        if(!result.immediate_status) {
            const DluKey key{ch,LocalDlu{cmd.address.local.value/dlu_bytes}};
            channels_[ch]->programs.try_emplace(key,stats_.accepted+1);
            if(result.disposition==WriteDisposition::BeginBlock)
                for(const auto& erase:writes_.pending_erases())
                    if(erase.logical==layout_.logical_view(cmd.address).block)
                        channels_[ch]->erases.emplace(erase.id,stats_.accepted+1);
        }
    } else {
        const auto result=reads_.submit(cmd);
        if(result.admission==SubmitResult::Backpressured) { wait(t,result.waits); return false; }
        t.read=result.candidate;
        if(t.read) t.target=capacity_.inspect(cmd.address); // Nonbinding route hint, no physical lease.
    }
    t.stage=Stage::Accepted;
    channels_[ch]->rx.release(cmd.op==Op::Write?cmd.size_bytes:0);
    t.input.data.clear(); t.input.data.shrink_to_fit(); // Assembly/Egress now owns accepted payload.
    t.accepted_sequence=++stats_.accepted; emit("accepted",ch,t.id.value);
    if(statistics_sink_)statistics_sink_->observe(StatAttemptAdvanced{t.id,AttemptMilestone::Accepted,events_.now()});
    if(t.acceptance) {
        const AcceptedNotice notice{t.id,ch,cmd.id,events_.now()};
        auto observer=std::move(t.acceptance);
        schedule(events_.now(),EventPhase::Arrival,[observer=std::move(observer),notice]{observer(notice);});
    }
    signals_.notify(WaitKey::channel_key(WaitReason::IngressSpace,ch));
    if(cmd.read_group) signals_.notify(WaitKey::order(ch,*cmd.read_group));
    if(cmd.op==Op::Read) {
        // Only real acceptance can close a Batch collection, including immediate
        // Pending forwarding/0xA and cache hits. Invalid read_hint has no Batch effect.
        if(t.read) try_cache(t);
        const bool member=t.read.has_value() && !t.cache_pending;
        auto hint=cmd.read_hint;
        if(hint!=ReadHint::Regular && hint!=ReadHint::Batch) hint=ReadHint::Regular;
        release_batches(ch,channels_[ch]->window.accept(member?std::optional<CommandId>{cmd.id}:std::nullopt,hint,events_.now()));
        update_timer(ch);
    }
    return true;
}
void DeviceDriver::release_batches(ChannelId ch,const std::vector<DispatchBatch>& batches) {
    for(const auto& batch:batches) {
        ++stats_.window_batches;
        for(auto command:batch) {
            auto* t=find(ch,command); if(t) { t->released=true; emit("window-release",ch,t->id.value); }
        }
    }
}
void DeviceDriver::update_timer(ChannelId ch) {
    auto& channel=*channels_[ch];
    if(channel.timer) { events_.cancel(*channel.timer); events_owned_.erase(*channel.timer); channel.timer.reset(); }
    const auto wake=channel.window.wakeup(); if(!wake) return;
    channel.timer=schedule(wake->at,EventPhase::Arrival,[this,ch,token=*wake] {
        auto& c=*channels_[ch]; c.timer.reset();
        const auto batch=c.window.flush_due(token,events_.now());
        if(batch) release_batches(ch,{*batch});
        update_timer(ch); request_pump(ch);
    });
}
void DeviceDriver::remove_window(Transaction& t) {
    const auto& cmd=t.input.command;
    if(channels_[cmd.address.channel]->window.remove(cmd.id)) update_timer(cmd.address.channel);
}
bool DeviceDriver::try_cache(Transaction& t) {
    if(!t.read || t.cache_pending || t.wait) return false;
    const auto c=*t.read;
    if(reads_.resolve_terminal(c) || !reads_.candidate(c.channel,c.command)) {
        remove_window(t); t.read.reset(); notify_sense_order(t); return true;
    }
    const auto probe=reads_.probe(c,false);
    if(probe.state==ProbeState::Blocked) { wait(t,probe.waits); return false; }
    if(probe.state!=ProbeState::Ready) return false;
    t.target=probe.target;
    auto result=cache_.pin(*probe.target); if(!result) return false;
    t.cache_result=std::move(result); t.cache_pending=true;
    notify_sense_order(t);
    remove_window(t); ++stats_.cache_hits; emit("cache-hit",c.channel,t.id.value,probe.target);
    const auto id=t.id.value;
    schedule(after(events_.now(),config_.media_timing.cache_access),EventPhase::Completion,[this,id,c] {
        auto& tx=transactions_.at(id);
        reads_.serve_cached(c,std::move(*tx.cache_result)); tx.cache_result.reset();
        tx.cache_pending=false; tx.read.reset(); notify_sense_order(tx);
        notify_cache(bank_of(*tx.target)); request_pump(c.channel);
    });
    return true;
}
void DeviceDriver::pump(ChannelId ch) {
    // Bounded active-command/Ingress directories only; never scan NAND or historical
    // mappings. Sleeping records are not retried unless their indexed condition fires.
    for(const auto identity:channels_[ch]->transactions) {
        auto& t=transactions_.at(identity);
        if(t.stage==Stage::Ingress && !t.wait) admit(t);
    }
    for(const auto identity:channels_[ch]->transactions) {
        auto& t=transactions_.at(identity); if(t.stage!=Stage::Accepted || !t.read) continue;
        // A path can independently settle an error while its candidate sleeps.
        if(!reads_.candidate(ch,t.input.command.id) && !t.cache_pending) {
            remove_window(t); t.read.reset(); notify_sense_order(t); if(t.wait) {signals_.cancel(*t.wait);t.wait.reset();}
        } else try_cache(t);
    }
    execute_media(ch);
    queue_responses(ch);
    start_response(ch);
}
void DeviceDriver::notify_sense_order(const Transaction& t) {
    if(t.target) signals_.notify(bank_wait(WaitReason::SenseOrder,bank_of(*t.target)));
}
bool DeviceDriver::prior_sense(const Transaction& t,BankKey bank) const {
    for(auto identity:channels_[bank.channel]->transactions) {
        const auto& older=transactions_.at(identity);
        if(!older.read || older.cache_pending || older.accepted_sequence>=t.accepted_sequence) continue;
        const auto p=reads_.probe(*older.read,false);
        if(p.state==ProbeState::Stale || p.state==ProbeState::Terminal) continue;
        const auto b=bank_of(capacity_.inspect(older.input.command.address));
        if(!(b<bank) && !(bank<b)) return true;
    }
    return false;
}
void DeviceDriver::execute_media(ChannelId ch) {
    // Rebuild the bounded READY set after each commit. Resource-blocked high priority
    // work is excluded, so another Bank can issue in this same Tick.
    struct Choice {
        MediaCandidateView view;
        std::optional<ReadCandidate> read;
        std::optional<ProgramCandidate> program;
        std::optional<CandidateId> erase;
        std::uint64_t transaction=0;
    };
    for(;;) {
        std::vector<Choice> choices;
        for(const auto& entry:channels_[ch]->erases) {
            const auto candidate=writes_.erase_candidate(entry.first);
            if(!candidate) throw std::logic_error("stale erase directory");
            const auto& e=*candidate;
            if(writes_.probe_erase(e.id).state!=ProbeState::Ready || !runtime_.available(bank_of(e.physical))) continue;
            const auto sequence=entry.second;
            choices.push_back({{choices.size()+1,sequence,MediaOperationKind::Erase,page_address(e.physical)},{},{},e.id,0});
        }
        auto& programs=channels_[ch]->programs;
        for(auto it=programs.begin();it!=programs.end();) {
            const auto c=writes_.program_candidate(it->first);
            if(!c) {it=programs.erase(it);continue;}
            const auto p=writes_.probe_program(*c);
            const auto a=writes_.program_target(*c);
            if(p.state==ProbeState::Ready && a && runtime_.available(bank_of(*a)))
                choices.push_back({{choices.size()+1,it->second,MediaOperationKind::Program,*a},{},c,{},0});
            ++it;
        }
        for(const auto identity:channels_[ch]->transactions) {
            auto& t=transactions_.at(identity);
            if(t.stage!=Stage::Accepted || !t.read || !t.released || t.cache_pending || t.wait) continue;
            const auto p=reads_.probe(*t.read,false);
            if(p.state==ProbeState::Terminal) { reads_.resolve_terminal(*t.read); continue; }
            if(p.state==ProbeState::Blocked) { wait(t,p.waits); continue; }
            if(p.state!=ProbeState::Ready) continue;
            t.target=p.target; const auto bank=bank_of(*p.target);
            // Another sense in this loop may make a formerly missed page a hit at
            // completion. Cache is rechecked by the next notification-driven pump.
            if(cache_.contains(*p.target)) { try_cache(t); continue; }
            if(prior_sense(t,bank)) { wait(t,{bank_wait(WaitReason::SenseOrder,bank)});continue; }
            if(!runtime_.available(bank)) { wait(t,{bank_wait(WaitReason::Bank,bank)});continue; }
            if(!cache_.can_fill(bank)) { ++stats_.cache_blocked;wait(t,{bank_wait(WaitReason::CacheSpace,bank)});continue; }
            choices.push_back({{choices.size()+1,t.accepted_sequence,MediaOperationKind::Read,*p.target},t.read,{},{},t.id.value});
        }
        if(choices.empty()) break;
        std::vector<MediaCandidateView> views; for(const auto& c:choices) views.push_back(c.view);
        const auto selected=nand_policy_->select(views);
        const auto it=std::find_if(choices.begin(),choices.end(),[&](const auto& c){return c.view.identity==selected;});
        if(it==choices.end()) throw std::logic_error("NAND policy selected a noncandidate");
        const auto choice=*it; const auto bank=bank_of(choice.view.target);
        // Includes topology validation/routing even though the Core link is ideal.
        (void)topology_.core_channel(bank.die);
        const auto ticket=runtime_.acquire(bank,choice.view.kind,events_.now());
        if(choice.erase) {
            const auto operation=writes_.issue_erase(*choice.erase);
            if(!operation) {runtime_.release(bank,ticket);throw std::logic_error("erase readiness changed during commit");}
            channels_[ch]->erases.erase(*choice.erase); ++stats_.erases;
            emit("erase-issue",ch,ticket,choice.view.target);
            schedule(after(events_.now(),config_.media_timing.erase),EventPhase::Completion,[this,op=*operation,bank,ticket] {
                cache_.invalidate(op.physical); writes_.finish_erase(op.id);
                runtime_.release(bank,ticket); notify_bank(bank); notify_cache(bank);
                emit("erase-finish",bank.channel,ticket,page_address(op.physical)); request_pump(bank.channel);
            });
        } else if(choice.program) {
            const auto operation=writes_.issue_program(*choice.program);
            if(!operation) {runtime_.release(bank,ticket);throw std::logic_error("program readiness changed during commit");}
            ++stats_.programs; emit("program-issue",ch,ticket,operation->physical);
            schedule(after(events_.now(),config_.media_timing.program),EventPhase::Completion,[this,op=*operation,bank,ticket] {
                const auto outcome=outcome_(op);
                // Actual media update invalidates tags, never protected response bytes.
                if(outcome==ProgramOutcome::Success) cache_.invalidate_page(op.physical);
                else cache_.invalidate(block_of(op.physical));
                writes_.finish_program(op.id,outcome);
                runtime_.release(bank,ticket); notify_bank(bank); notify_cache(bank);
                emit("program-finish",bank.channel,ticket,op.physical); request_pump(bank.channel);
            });
        } else {
            auto fill=cache_.begin_fill(choice.view.target,media_.read(choice.view.target));
            if(!fill) {runtime_.release(bank,ticket);throw std::logic_error("Cache readiness changed during commit");}
            auto result=cache_.pin_fill(*fill,choice.view.target.byte_offset);
            const auto operation=reads_.issue_reserved(*choice.read,std::move(result));
            if(!operation) {cache_.cancel_fill(*fill);runtime_.release(bank,ticket);continue;}
            auto& tx=transactions_.at(choice.transaction); remove_window(tx); tx.read.reset(); notify_sense_order(tx);
            ++stats_.senses; emit("sense-issue",ch,ticket,operation->physical);
            schedule(after(events_.now(),config_.media_timing.sense),EventPhase::Completion,[this,op=*operation,fill=*fill,bank,ticket] {
                cache_.finish_fill(fill); reads_.finish_read(op.id);
                runtime_.release(bank,ticket); notify_bank(bank); notify_cache(bank);
                emit("sense-finish",bank.channel,ticket,op.physical); request_pump(bank.channel);
            });
        }
    }
}
void DeviceDriver::queue_responses(ChannelId ch) {
    for(const auto identity:channels_[ch]->transactions) {
        auto& t=transactions_.at(identity); const auto& cmd=t.input.command;
        if(t.stage!=Stage::Accepted || t.cache_pending) continue;
        if(cmd.op==Op::Read) {
            const auto response=reads_.response(ch,cmd.id); if(!response) continue;
            if(response->data) {
                auto slot=reads_.result_in(ch,cmd.id,egress_);
                if(!slot) {
                    slot=egress_.reserve(ch); if(!slot) continue; // Cache retains the result.
                    reads_.transfer_result(ch,cmd.id,ReadResultLease(egress_,*slot));
                    if(t.target) notify_cache(bank_of(*t.target));
                }
                t.egress=slot; t.egress_in_path=true;
            } else {
                t.egress=egress_.reserve_metadata(ch); if(!t.egress) continue;
            }
        } else {
            if(!writes_.response(ch,cmd.id)) continue;
            t.egress=egress_.reserve_metadata(ch); if(!t.egress) continue;
        }
        if(t.wait) {signals_.cancel(*t.wait);t.wait.reset();}
        remove_window(t); t.read.reset(); t.stage=Stage::Egress;
        emit("egress-ready",ch,t.id.value);
    }
}
void DeviceDriver::start_response(ChannelId ch) {
    auto& channel=*channels_[ch]; if(channel.response_busy) return;
    std::vector<TransferCandidateView> candidates;
    for(const auto identity:channels_[ch]->transactions) {
        const auto& t=transactions_.at(identity); const auto& cmd=t.input.command;
        if(t.stage!=Stage::Egress) continue;
        const auto payload=cmd.op==Op::Read && reads_.response(ch,cmd.id)->data?host_unit_bytes:0;
        candidates.push_back({t.id.value,t.accepted_sequence,cmd.op,checked_add(config_.ucie.overhead.response_control_bytes,payload)});
    }
    if(candidates.empty()) return;
    const auto id=link_policy_->select(candidates);
    const auto chosen=std::find_if(candidates.begin(),candidates.end(),[&](const auto& c){return c.identity==id;});
    if(chosen==candidates.end()) throw std::logic_error("link policy selected a noncandidate");
    auto& t=transactions_.at(id); t.stage=Stage::ResponseLink;
    channel.response_busy=true; ++channel.response_flights;
    const auto serialized=after(events_.now(),config_.ucie.serialization(chosen->service_bytes));
    schedule(serialized,EventPhase::Completion,[this,ch] { channels_[ch]->response_busy=false; request_pump(ch); });
    schedule(after(serialized,config_.ucie.overhead.response_fixed_latency),EventPhase::Completion,[this,id]{deliver(id);});
    emit("response-issue",ch,id);
}
void DeviceDriver::deliver(std::uint64_t id) {
    auto& t=transactions_.at(id); const auto& cmd=t.input.command; const auto ch=cmd.address.channel;
    if(t.stage!=Stage::ResponseLink) throw std::logic_error("duplicate response delivery");
    if(cmd.op==Op::Read) {
        t.delivered=*reads_.response(ch,cmd.id); reads_.response_delivered(ch,cmd.id);
    } else {
        t.delivered=*writes_.response(ch,cmd.id); writes_.response_delivered(ch,cmd.id);
    }
    if(!t.egress_in_path) egress_.release(*t.egress);
    t.egress.reset(); t.stage=Stage::Feedback;
    auto& channel=*channels_[ch]; --channel.response_flights; ++channel.feedback;
    ++stats_.delivered; emit("delivered",ch,id);
    if(statistics_sink_)std::visit([&](const auto& response) {
        statistics_sink_->observe(StatAttemptDelivered{t.id,events_.now(),cmd.op,static_cast<std::uint8_t>(response.status)});
    },*t.delivered);
    signals_.notify(WaitKey::channel_key(WaitReason::ResultSpace,0));
    if(t.target) notify_cache(bank_of(*t.target));
    // Cleanup first; feedback is a subsequent normal event, never a recursive call.
    const auto delivery_time=events_.now();
    schedule(delivery_time,EventPhase::Arrival,[this,id,ch,delivery_time] {
        auto& tx=transactions_.at(id); const auto command=tx.input.command.id;
        DeliveredResponse response{tx.id,std::move(*tx.delivered),delivery_time};
        auto receiver=std::move(tx.receiver);
        identities_.erase({ch,command}); channels_[ch]->transactions.erase(id);
        transactions_.erase(id); --channels_[ch]->feedback;
        receiver(response); // Host owns delivered symbolic bytes from this boundary.
    });
    request_pump(ch);
}
DeviceActivity DeviceDriver::activity() const {
    DeviceActivity a;
    a.unsettled_transfers=transactions_.size(); a.outstanding=admission_.total_outstanding();
    a.protected_cache_slots=cache_.protected_slots(); a.filling_cache_slots=cache_.filling_slots();
    a.scheduled_driver_events=events_owned_.size(); a.queued_wakeups=signals_.queued_wakeups();
    a.bank_operations=runtime_.active(); a.waiting_erases=writes_.waiting_erases(); a.queued_reads=reads_.waiting();
    for(ChannelId ch=0;ch<channels_.size();++ch) {
        const auto& c=*channels_[ch];
        a.ingress+=c.rx.entries(); a.in_flight_requests+=c.request_flights;
        a.pending_dlus+=writes_.pending(ch); a.window_reads+=c.window.size();
        a.egress+=egress_.budget(ch).entries(); a.in_flight_responses+=c.response_flights; a.feedback_events+=c.feedback;
    }
    return a; // Persistent cache/media/mapping state is deliberately excluded.
}
void DeviceDriver::declare_discardable(ChannelId ch,std::uint64_t z) { capacity_.declare_discardable(ch,z); }
bool DeviceDriver::quiesce_pair(ChannelId ch,std::uint64_t a,std::uint64_t b) { return capacity_.quiesce_pair(ch,a,b); }
void DeviceDriver::cancel_quiesce(ChannelId ch,std::uint64_t a,std::uint64_t b) { capacity_.cancel_quiesce(ch,a,b); request_pump(ch); }
RemapResult DeviceDriver::commit_swap(ChannelId ch,std::uint64_t a,std::uint64_t b) {
    const auto result=capacity_.commit_swap(ch,a,b,[this]{cache_.invalidate_all();});
    if(result==RemapResult::Applied) { request_pump(ch); emit("zone-swap",ch,0); }
    return result;
}
} // namespace hbf_eventlab

