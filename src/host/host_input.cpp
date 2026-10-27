#include "hbf_eventlab/host/host_driver.hpp"
#include <algorithm>

namespace hbf_eventlab {
void HostDriver::set_completion_handler(std::function<void(const GlobalCompletion&)> handler) {
    if(loaded_ || started_ || completion_ || !handler)
        throw std::logic_error("attach a nonempty Host completion handler once before load/start");
    completion_=std::move(handler);
}

std::vector<HostDriver::PreparedRequest> HostDriver::prepare(const std::vector<GlobalRequest>& input) const {
    RuntimeScope timing(profile_,RuntimeRegion::Prepare);
    if(profile_)profile_->add(RuntimeCounter::PrepareGlobals,input.size());
    // All input-dependent rejection precedes registration, statistics, and events.
    // Allocation failures or exceptions from user observers are fatal, not a
    // recoverable rejected submission. Never rebuild the full workload on append.
    std::map<GlobalRequestId,const GlobalRequest*> ids;
    std::uint64_t count=next_work_;
    for(const auto& r:input) {
        if(results_.count(r.id) || !ids.emplace(r.id,&r).second)
            throw std::invalid_argument("duplicate Global ID");
        if(r.op!=Op::Read && r.op!=Op::Write)throw std::invalid_argument("invalid operation");
        if(r.op==Op::Write&&!r.after.empty())throw std::invalid_argument("LIMIT-WRITE-001: extra write dependencies unsupported");
        if(r.op==Op::Write&&(r.group||r.expected_content))throw std::invalid_argument("read-only fields on Write");
        if(!r.bytes||r.bytes%host_unit_bytes||r.address.value%host_unit_bytes)
            throw std::invalid_argument("unaligned/empty Global range");
        (void)checked_add(r.address.value,r.bytes);
        count=checked_add(count,r.bytes/host_unit_bytes);
        if(config_.max_logical_fragments && count>config_.max_logical_fragments)
            throw std::invalid_argument("workload exceeds configured fragment limit");
        if(r.not_before<events_.now())throw std::invalid_argument("workload release time is in the past");
        if(r.offered_at && r.not_before<*r.offered_at)throw std::invalid_argument("producer offer time exceeds release time");
        if(r.offered_at && events_.now()<*r.offered_at)throw std::invalid_argument("producer offer time exceeds registration time");
    }
    std::map<GlobalRequestId,int> color;
    std::function<void(GlobalRequestId)> visit=[&](GlobalRequestId id) {
        // Previously committed dependencies cannot point at newly introduced IDs.
        if(results_.count(id))return;
        if(!ids.count(id))throw std::invalid_argument("unknown read dependency");
        if(color[id]==1)throw std::invalid_argument("cyclic read dependency");
        if(color[id]==2)return;
        color[id]=1;for(auto dependency:ids.at(id)->after)visit(dependency);color[id]=2;
    };
    for(const auto& r:input)visit(r.id);

    std::map<LogicalBlockKey,std::uint64_t> current;
    std::set<ContentKey> newly_seen;
    std::map<PageKey,std::bitset<fragments_per_dlu>> coverage;
    std::vector<PreparedRequest> prepared;
    for(const auto& r:input) {
        PreparedRequest item{r,splitter_.split(r)};
        for(const auto& f:item.fragments) {
            if(f.address.channel>=channels_.size() || f.address.local.value>=port_.description().geometry().channel_bytes())
                throw std::invalid_argument("interleaver outside channel geometry");
            if(r.op!=Op::Write)continue;
            const ContentKey key{f.logical.block,r.content};
            auto cur=current.find(key.block);
            if(cur==current.end()) {
                const auto order=version_order_.find(key.block);
                cur=current.emplace(key.block,order==version_order_.end()?key.content:order->second.back().content).first;
            }
            const auto existing=versions_.find(key);
            if(cur->second!=key.content) {
                if(existing!=versions_.end() || newly_seen.count(key))
                    throw std::invalid_argument("LIMIT-CONTENT-001: reused noncontiguous content version (Host input version sealed)");
                cur->second=key.content;
            }
            newly_seen.insert(key);
            if(existing!=versions_.end() && existing->second.aborted)
                throw std::invalid_argument("Host input content version already aborted");
            const auto slot=f.logical.byte_offset/host_unit_bytes;
            if(existing!=versions_.end()) {
                const auto page=existing->second.defined.find(f.logical.page);
                if(page!=existing->second.defined.end() && page->second.test(slot))
                    throw std::invalid_argument("LIMIT-CONTENT-002: duplicate write fragment in one content version");
            }
            auto& bits=coverage[{key,f.logical.page}];
            if(bits.test(slot))throw std::invalid_argument("LIMIT-CONTENT-002: duplicate write fragment in one content version");
            bits.set(slot);
        }
        prepared.push_back(std::move(item));
    }
    if(config_.writes==WriteCommandMode::Burst4096)
        for(const auto& page:coverage)if(!page.second.all())
            throw std::invalid_argument("BURST-INCOMPLETE: explicit burst mode needs complete local DLU coverage in one submission");
    return prepared;
}

void HostDriver::register_input(std::vector<PreparedRequest> input) {
    RuntimeScope timing(profile_,RuntimeRegion::RegisterInput);
    std::map<PageKey,std::vector<Fragment>> burst;
    std::vector<PageKey> burst_order;
    std::vector<GlobalRequestId> added;
    std::map<std::pair<GlobalRequestId,ChannelId>,SendUnitId> tails;
    for(auto& item:input) {
        const auto& r=item.request;
        added.push_back(r.id);
        requests_.emplace(r.id,r);
        results_.emplace(r.id,WorkResult{r.id,LogicalResult::Pending,static_cast<std::size_t>(r.bytes/host_unit_bytes)});
        if(profile_) {profile_->add(RuntimeCounter::RegisteredGlobals);profile_->peak(RuntimeCounter::ResultsSizePeak,results_.size());}
        if(statistics_sink_)statistics_sink_->observe(StatGlobalDefined{r.id,r.op,r.bytes,r.not_before});
        if(statistics_sink_ && r.offered_at)statistics_sink_->observe(StatInputRegistered{r.id,*r.offered_at,events_.now()});
        for(auto& f:item.fragments) {
            next_work_=checked_add(next_work_,1);f.id=LogicalWorkId{next_work_};work_.emplace(f.id,Work{f});
            if(profile_)profile_->add(RuntimeCounter::RegisteredFragments);
            if(statistics_sink_)statistics_sink_->observe(StatWorkDefined{f.id,r.id,f.address,
                r.op==Op::Write?std::optional<std::uint64_t>{r.content}:std::nullopt});
            std::optional<ContentKey> content;
            if(r.op==Op::Write) {
                content=ContentKey{f.logical.block,r.content};
                if(!versions_.count(*content)) {
                    versions_.emplace(*content,Version{});
                    // Only successful batch registration changes the current version.
                    // Older entries remain for in-flight work and internal recovery.
                    version_order_[content->block].push_back(*content);
                }
                versions_.at(*content).defined[f.logical.page].set(f.logical.byte_offset/host_unit_bytes);
                ++versions_.at(*content).unfinished;
            }
            if(r.op==Op::Write && config_.writes==WriteCommandMode::Burst4096) {
                const PageKey key{*content,f.logical.page};
                if(!burst.count(key))burst_order.push_back(key);
                burst[key].push_back(f);
            } else {
                const auto unit=add_unit(r.op,{f},content,r.group);
                if(r.op==Op::Read)tails[{r.id,f.address.channel}]=unit;
            }
        }
    }
    for(auto key:burst_order) {
        auto fragments=std::move(burst.at(key));
        std::sort(fragments.begin(),fragments.end(),[](const auto&a,const auto&b){return a.logical.byte_offset<b.logical.byte_offset;});
        add_unit(Op::Write,std::move(fragments),key.content,{});
    }
    if(config_.hints==ReadHintMode::TailBatch)
        for(const auto& tail:tails)units_.at(tail.second).hint=ReadHint::Batch;
    for(auto& release:initial_enqueues_)std::sort(release.second.begin(),release.second.end());
    if(started_)arm_releases(added);
}

void HostDriver::load(const std::vector<GlobalRequest>& input) {
    if(loaded_||started_)throw std::logic_error("Host workload already loaded");
    auto prepared=prepare(input);std::uint64_t count=0;
    for(const auto& p:prepared)count=checked_add(count,p.fragments.size());
    if(config_.max_resident_fragments && count>config_.max_resident_fragments)
        throw std::invalid_argument("finite workload exceeds resident input budget; use incremental try_submit");
    register_input(std::move(prepared));
    loaded_=true;
}
void HostDriver::submit(const std::vector<GlobalRequest>& input) {
    const auto r=try_submit(input);
    if(r.disposition!=InputDisposition::Registered)
        throw std::invalid_argument(r.disposition==InputDisposition::TooLarge?"input batch exceeds resident budget":"input backpressured; use try_submit");
}
void HostDriver::start_incremental(const std::vector<GlobalRequest>& initial) {
    const auto r=try_start_incremental(initial);
    if(r.disposition!=InputDisposition::Registered)throw std::invalid_argument("initial input batch exceeds resident budget");
}
void HostDriver::start() {
    if(!loaded_||started_)throw std::logic_error("Host must load exactly once before start");
    started_=true;
    std::vector<GlobalRequestId> ids;
    for(const auto& r:requests_)ids.push_back(r.first);
    arm_releases(ids);
}
void HostDriver::arm_releases(const std::vector<GlobalRequestId>& ids) {
    release_units(events_.now());
    for(auto id:ids) {
        const auto at=requests_.at(id).not_before;
        if(!(events_.now()<at)) {
            if(statistics_sink_)statistics_sink_->observe(StatGlobalReleased{id,at});
        } else {
            release_globals_[at].push_back(id);
            if(release_events_.count(at))continue;
            release_events_[at]=schedule(at,[this,at] {
                release_units(at);
                const auto ids=std::move(release_globals_.at(at));release_globals_.erase(at);
                if(statistics_sink_)for(auto id:ids)
                    if(results_.at(id).state==LogicalResult::Pending)
                        statistics_sink_->observe(StatGlobalReleased{id,at});
                release_events_.erase(at);
                for(ChannelId ch=0;ch<channels_.size();++ch)request_pump(ch);
            });
        }
    }
    for(ChannelId ch=0;ch<channels_.size();++ch)request_pump(ch);
}
} // namespace hbf_eventlab
