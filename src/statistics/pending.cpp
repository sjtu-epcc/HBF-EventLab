#include "hbf_eventlab/statistics/collector.hpp"
#include <algorithm>
#include <ostream>
#include <stdexcept>

namespace hbf_eventlab {
namespace {
void require(bool ok,const char* why){if(!ok)throw std::logic_error(why);}
std::uint64_t& phase_count(PendingCounts& counts,WritePhase phase) {
    switch(phase) {
    case WritePhase::Collecting:return counts.collecting;
    case WritePhase::Ready:case WritePhase::ProgramQueued:return counts.ready_waiting;
    case WritePhase::Programming:return counts.programming;
    }
    throw std::logic_error("statistics: invalid write phase");
}
void integrate(PendingSummary& s,Tick from,Tick to) {
    require(!(to<from),"statistics: Pending time moved backwards");
    const auto dt=to.value-from.value;
    s.area.total+=static_cast<long double>(s.current.total)*dt;
    s.area.collecting+=static_cast<long double>(s.current.collecting)*dt;
    s.area.ready_waiting+=static_cast<long double>(s.current.ready_waiting)*dt;
    s.area.programming+=static_cast<long double>(s.current.programming)*dt;
    if(s.current.total==s.limit)s.at_limit_ticks=checked_add(s.at_limit_ticks,dt);
}
bool equal(const PendingCounts& a,const PendingCounts& b) {
    return std::tie(a.total,a.collecting,a.ready_waiting,a.programming)==std::tie(b.total,b.collecting,b.ready_waiting,b.programming);
}
const char* phase_name(WritePhase p) {
    switch(p){case WritePhase::Collecting:return "collecting";case WritePhase::Ready:return "ready";
        case WritePhase::ProgramQueued:return "program_queued";case WritePhase::Programming:return "programming";}
    throw std::logic_error("statistics: invalid write phase");
}
const char* change_name(AssemblyChange c) {
    switch(c){case AssemblyChange::Opened:return "opened";case AssemblyChange::Progress:return "progress";
        case AssemblyChange::PhaseChanged:return "phase_changed";case AssemblyChange::Retired:return "retired";}
    throw std::logic_error("statistics: invalid assembly change");
}
template<class T> void counts_json(std::ostream& out,const T& c) {
    out<<"{\"total\":"<<c.total<<",\"collecting\":"<<c.collecting<<",\"ready_waiting\":"<<c.ready_waiting<<",\"programming\":"<<c.programming<<'}';
}
void time_json(std::ostream& out,std::optional<Tick> t){if(t)out<<t->value;else out<<"null";}
void duration_json(std::ostream& out,std::optional<Tick> end,std::optional<Tick> begin) {
    if(end&&begin)out<<end->value-begin->value;else out<<"null";
}
} // namespace
const char* to_string(DluTermination reason) {
    switch(reason){case DluTermination::ProgramSuccess:return "program_success";case DluTermination::ProgramFail:return "program_fail";
        case DluTermination::PriorProgramFail:return "prior_program_fail";case DluTermination::AssemblyTimeout:return "assembly_timeout";}
    throw std::logic_error("statistics: invalid DLU termination");
}
void StatisticsCollector::record(const StatPendingInitialized& e) {
    require(e.limit && !pending_.count(e.channel),"statistics: duplicate/invalid Pending initialization");
    PendingState state;state.summary.observed_from=e.at;state.summary.limit=e.limit;state.last=e.at;
    pending_.emplace(e.channel,state);
    next_pending_sequence_=checked_add(next_pending_sequence_,1);
    pending_timeline_.push_back({e.at,next_pending_sequence_,e.channel,{}});note(e.at);
}
void StatisticsCollector::record(const StatDluObserved& event) {
    const auto& e=event.observation;auto& channel=pending_.at(e.key.channel);
    require(!(event.at<channel.last)&&!(event.at<e.first_arrival)&&e.generation.value&&
            e.received_fragments&&e.received_fragments<=fragments_per_dlu,"statistics: invalid DLU observation");
    const DluContextKey key{e.key.channel,e.key.dlu.value,e.generation.value};
    const bool opening=e.change==AssemblyChange::Opened,retiring=e.change==AssemblyChange::Retired;
    require(retiring==e.status.has_value(),"statistics: terminal status outside retirement");
    if(opening) {
        require(!dlus_.count(key)&&!live_dlus_.count(e.key)&&event.at==e.first_arrival&&
                e.phase==WritePhase::Collecting&&e.received_fragments==1&&e.command.has_value()&&
                channel.summary.current.total<channel.summary.limit,"statistics: invalid DLU open");
    } else {
        const auto& d=dlus_.at(key);const auto live=live_dlus_.find(e.key);
        require(!d.retired_at&&live!=live_dlus_.end()&&live->second==e.generation&&
                d.opened_at==e.first_arrival&&d.deadline==e.deadline,"statistics: stale/changed DLU identity");
        if(e.change==AssemblyChange::Progress) {
            require(d.phase==WritePhase::Collecting&&e.command.has_value()&&e.received_fragments==d.received_fragments+1&&
                e.phase==(e.received_fragments==fragments_per_dlu?WritePhase::Ready:WritePhase::Collecting),"statistics: invalid DLU progress");
        } else if(e.change==AssemblyChange::PhaseChanged) {
            require(!e.command&&e.received_fragments==d.received_fragments&&
                ((d.phase==WritePhase::Ready&&e.phase==WritePhase::ProgramQueued)||
                 (d.phase==WritePhase::ProgramQueued&&e.phase==WritePhase::Programming)),"statistics: invalid DLU phase change");
        } else require(retiring&&!e.command&&e.phase==d.phase&&e.received_fragments==d.received_fragments,"statistics: invalid DLU retirement");
    }
    std::optional<DluTermination> termination;
    if(retiring) {
        if(*e.status==WriteStatus::Success&&e.phase==WritePhase::Programming&&!e.additional_info)termination=DluTermination::ProgramSuccess;
        else if(*e.status==WriteStatus::ProgramFail&&e.phase==WritePhase::Programming&&!e.additional_info)termination=DluTermination::ProgramFail;
        else if(*e.status==WriteStatus::WriteOrderViolation&&e.additional_info==2&&e.phase!=WritePhase::Programming)termination=DluTermination::PriorProgramFail;
        else if(*e.status==WriteStatus::AssemblyTimeout&&e.phase==WritePhase::Collecting&&!e.additional_info&&!(event.at<e.deadline))termination=DluTermination::AssemblyTimeout;
        require(termination.has_value(),"statistics: unknown/inconsistent DLU termination");
    }
    // These are measurement invariants, not readiness/assembly decisions.
    const auto before=channel.summary.current;
    integrate(channel.summary,channel.last,event.at);channel.last=event.at;
    if(opening) {
        DluTiming d;d.key=e.key;d.generation=e.generation;d.opened_at=e.first_arrival;d.deadline=e.deadline;
        dlus_.emplace(key,std::move(d));live_dlus_.emplace(e.key,e.generation);
        ++channel.summary.current.total;++channel.summary.opened;
    } else {
        auto& count=phase_count(channel.summary.current,dlus_.at(key).phase);
        require(count>0,"statistics: negative Pending phase count");--count;
    }
    auto& d=dlus_.at(key);
    d.phase=e.phase;d.received_fragments=e.received_fragments;
    if(retiring) {
        --channel.summary.current.total;++channel.summary.retired;live_dlus_.erase(e.key);
        d.retired_at=event.at;d.termination=termination;d.status=e.status;d.additional_info=e.additional_info;
    } else {
        ++phase_count(channel.summary.current,e.phase);
        if(e.phase==WritePhase::Ready&&!d.ready_at)d.ready_at=event.at;
        if(e.phase==WritePhase::Programming&&!d.program_started_at)d.program_started_at=event.at;
    }
    if(e.command&&!d.members.count(*e.command)) {
        const auto found=command_attempts_.find({e.key.channel,*e.command});
        d.members.emplace(*e.command,found==command_attempts_.end()?std::nullopt:std::optional<TransferAttemptId>{found->second});
    }
    next_pending_sequence_=checked_add(next_pending_sequence_,1);
    d.progress.push_back({event.at,next_pending_sequence_,e.change,e.phase,e.received_fragments,e.command});
    const auto& c=channel.summary.current;auto& peak=channel.summary.peak;
    require(c.total==c.collecting+c.ready_waiting+c.programming&&c.total<=channel.summary.limit,"statistics: Pending count invariant");
    peak.total=std::max(peak.total,c.total);peak.collecting=std::max(peak.collecting,c.collecting);
    peak.ready_waiting=std::max(peak.ready_waiting,c.ready_waiting);peak.programming=std::max(peak.programming,c.programming);
    if(!equal(before,c))pending_timeline_.push_back({event.at,next_pending_sequence_,e.key.channel,c});
    note(event.at);
}
std::map<ChannelId,PendingSummary> StatisticsCollector::pending_summary(Tick at) const {
    std::map<ChannelId,PendingSummary> result;
    for(const auto& entry:pending_) {
        auto s=entry.second.summary;integrate(s,entry.second.last,at);
        const auto dt=at.value-s.observed_from.value;
        if(dt){s.mean={s.area.total/dt,s.area.collecting/dt,s.area.ready_waiting/dt,s.area.programming/dt};}
        result.emplace(entry.first,s);
    }
    return result;
}
void StatisticsCollector::write_pending_json(std::ostream& out,Tick at) const {
    out<<",\"pending_channels\":[";bool comma=false;
    for(const auto& entry:pending_summary(at)) {
        if(comma)out<<',';comma=true;const auto& s=entry.second;
        out<<"{\"channel\":"<<entry.first<<",\"limit\":"<<s.limit<<",\"observed_from\":"<<s.observed_from.value
           <<",\"observed_until\":"<<at.value<<",\"opened\":"<<s.opened<<",\"retired\":"<<s.retired
           <<",\"at_limit_ticks\":"<<s.at_limit_ticks<<",\"current\":";counts_json(out,s.current);
        out<<",\"peak\":";counts_json(out,s.peak);out<<",\"area_context_ticks\":";counts_json(out,s.area);
        out<<",\"time_weighted_mean\":";counts_json(out,s.mean);out<<'}';
    }
    out<<"],\"pending_timeline\":[";comma=false;
    for(const auto& s:pending_timeline_) {
        if(comma)out<<',';comma=true;
        out<<"{\"at\":"<<s.at.value<<",\"sequence\":"<<s.sequence<<",\"channel\":"<<s.channel<<",\"counts\":";
        counts_json(out,s.counts);out<<'}';
    }
    out<<"],\"dlu_contexts\":[";comma=false;
    for(const auto& entry:dlus_) {
        if(comma)out<<',';comma=true;const auto& d=entry.second;
        out<<"{\"channel\":"<<d.key.channel<<",\"local_dlu\":"<<d.key.dlu.value<<",\"generation\":"<<d.generation.value
           <<",\"phase\":\""<<phase_name(d.phase)<<"\",\"active\":"<<(d.retired_at?"false":"true")
           <<",\"received_fragments\":"<<d.received_fragments<<",\"opened_at\":"<<d.opened_at.value<<",\"deadline\":"<<d.deadline.value
           <<",\"ready_at\":";time_json(out,d.ready_at);out<<",\"program_started_at\":";time_json(out,d.program_started_at);
        out<<",\"retired_at\":";time_json(out,d.retired_at);out<<",\"termination\":";
        if(d.termination)out<<'"'<<to_string(*d.termination)<<'"';else out<<"null";
        out<<",\"status\":";if(d.status)out<<unsigned(*d.status);else out<<"null";
        out<<",\"additional_info\":";if(d.status)out<<unsigned(d.additional_info);else out<<"null";
        out<<",\"assembly_ticks\":";duration_json(out,d.ready_at,d.opened_at);
        out<<",\"ready_wait_ticks\":";duration_json(out,d.program_started_at,d.ready_at);
        out<<",\"program_ticks\":";duration_json(out,d.retired_at,d.program_started_at);
        out<<",\"residence_ticks\":";duration_json(out,d.retired_at,d.opened_at);
        out<<",\"observed_residence_ticks\":"<<(d.retired_at?d.retired_at->value:at.value)-d.opened_at.value;
        out<<",\"members\":[";bool sep=false;
        for(const auto& m:d.members){if(sep)out<<',';sep=true;out<<"{\"command\":"<<m.first.value<<",\"attempt\":";
            if(m.second)out<<m.second->value;else out<<"null";out<<'}';}
        out<<"],\"progress\":[";sep=false;
        for(const auto& p:d.progress){if(sep)out<<',';sep=true;
            out<<"{\"at\":"<<p.at.value<<",\"sequence\":"<<p.sequence<<",\"change\":\""<<change_name(p.change)
               <<"\",\"phase\":\""<<phase_name(p.phase)<<"\",\"received_fragments\":"<<p.received_fragments<<",\"command\":";
            if(p.command)out<<p.command->value;else out<<"null";out<<'}';}
        out<<"]}";
    }
    out<<']';
}
} // namespace hbf_eventlab
