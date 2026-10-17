#include "hbf_eventlab/statistics/collector.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace hbf_eventlab {
namespace {
void require(bool ok,const char* message) {if(!ok)throw std::logic_error(message);}
std::uint64_t elapsed(Tick end,Tick start) {
    require(!(end<start),"statistics: negative duration");return end.value-start.value;
}
const char* op_name(Op op) {return op==Op::Read?"read":"write";}
const char* outcome(LogicalResult r) {
    return r==LogicalResult::Success?"success":r==LogicalResult::Failed?"failed":"pending";
}
void count(ResultCounts& c,LogicalResult r,bool cancelled=false) {
    ++c.total;
    if(r==LogicalResult::Success)++c.success;
    else if(r==LogicalResult::Failed)++c.failed;
    else ++c.pending;
    if(cancelled)++c.cancelled_before_release;
}
LatencySummary summarize_samples(std::vector<std::uint64_t>& v) {
    std::sort(v.begin(),v.end());LatencySummary s;
    s.count=v.size();s.min=v.front();s.max=v.back();
    long double total=0;for(auto n:v)total+=n;s.mean=total/v.size();
    // Nearest-rank percentiles, including a one-sample distribution.
    const auto rank=[&](std::size_t p){return v[(v.size()/100)*p+(v.size()%100*p+99)/100-1];};
    s.p50=rank(50);s.p95=rank(95);s.p99=rank(99);return s;
}
std::string quote(const std::string& s) {
    std::ostringstream out;out<<'"';
    for(unsigned char c:s) {
        if(c=='"'||c=='\\')out<<'\\'<<c;
        else if(c<0x20)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
void time_json(std::ostream& out,std::optional<Tick> t) {if(t)out<<t->value;else out<<"null";}
void counts_json(std::ostream& out,const std::map<Op,ResultCounts>& counts) {
    out<<'{';bool comma=false;
    for(auto op:{Op::Read,Op::Write}) {
        if(comma)out<<',';comma=true;const auto& c=counts.at(op);
        out<<quote(op_name(op))<<":{\"total\":"<<c.total<<",\"success\":"<<c.success
           <<",\"failed\":"<<c.failed<<",\"pending\":"<<c.pending
           <<",\"cancelled_before_release\":"<<c.cancelled_before_release<<'}';
    }
    out<<'}';
}
} // namespace
void StatisticsCollector::note(Tick t) {if(latest_<t)latest_=t;}
void StatisticsCollector::observe(const StatisticsEvent& event) {
    std::visit([this](const auto& e){record(e);},event);
}
void StatisticsCollector::record(const StatGlobalDefined& e) {
    require(e.bytes && e.bytes%host_unit_bytes==0,"statistics: invalid Global size");
    require(globals_.emplace(e.id,GlobalTiming{e.id,e.op,e.bytes,e.release}).second,"statistics: duplicate Global");
}
void StatisticsCollector::record(const StatWorkDefined& e) {
    auto& parent=globals_.at(e.parent);
    require(!parent.released && !parent.finished,"statistics: late work registration");
    require(work_.emplace(e.id,LogicalTiming{e.id,e.parent,e.address,e.content}).second,"statistics: duplicate work");
    ++parent.children;++parent.remaining;
    if(e.content)require(write_index_.emplace(std::make_tuple(e.address.channel,e.address.local.value,*e.content),e.id).second,
                         "statistics: duplicate write content fragment");
}
void StatisticsCollector::record(const StatGlobalReleased& e) {
    auto& g=globals_.at(e.id);
    require(!g.released && !g.finished && !(e.at<g.scheduled_release),"statistics: invalid release");
    g.released=e.at;note(e.at);
}
void StatisticsCollector::record(const StatWorkFinished& e) {
    auto& w=work_.at(e.id);auto& g=globals_.at(w.parent);
    require(!w.finished && g.remaining,"statistics: duplicate logical completion");
    if(g.released)(void)elapsed(e.at,*g.released);
    require(!e.success || g.released.has_value(),"statistics: success before release");
    w.finished=e.at;w.result=e.success?LogicalResult::Success:LogicalResult::Failed;w.reason=e.reason;
    // A Global can span blocks: one child's version can be aborted before
    // release, while other children still release normally at a later Tick.
    w.cancelled_before_release=!g.released;
    --g.remaining;if(!e.success)++g.failed;
    if(!g.last_child_finished || *g.last_child_finished<e.at)g.last_child_finished=e.at;
    note(e.at);
}
void StatisticsCollector::record(const StatGlobalFinished& e) {
    auto& g=globals_.at(e.id);
    require(!g.finished && !g.remaining && g.children==g.bytes/host_unit_bytes &&
            g.last_child_finished==e.at && e.success==!g.failed,"statistics: invalid Global completion");
    g.finished=e.at;g.result=e.success?LogicalResult::Success:LogicalResult::Failed;g.reason=e.reason;note(e.at);
}
void StatisticsCollector::record(const StatAttemptStarted& e) {
    require(attempts_.emplace(e.id,AttemptTiming{e.id,e.command,e.at}).second,"statistics: duplicate attempt");note(e.at);
}
void StatisticsCollector::record(const StatAttemptLinked& e) {
    auto& a=attempts_.at(e.id);require(!a.unit && e.unit_attempt,"statistics: duplicate/invalid attempt origin");
    (void)elapsed(a.handoff,e.queued_at);
    auto work=e.work;
    if(e.replay) {
        require(e.content.has_value() && work.empty(),"statistics: invalid replay origin");
        for(std::uint64_t offset=0;offset<a.command.size_bytes;offset+=host_unit_bytes)
            work.push_back(write_index_.at({a.command.address.channel,a.command.address.local.value+offset,*e.content}));
    }
    require(work.size()==a.command.size_bytes/host_unit_bytes,"statistics: incomplete command origin");
    for(auto id:work) {
        const auto& w=work_.at(id);const auto& g=globals_.at(w.parent);
        require(g.released.has_value() && g.op==a.command.op,"statistics: attempt before logical release");
        (void)elapsed(a.handoff,*g.released);
    }
    a.unit=e.unit;a.queued_at=e.queued_at;a.unit_attempt=e.unit_attempt;
    a.replay=e.replay;a.replay_pass=e.replay_pass;a.work=std::move(work);
}
void StatisticsCollector::record(const StatAttemptAdvanced& e) {
    auto& a=attempts_.at(e.id);
    if(e.stage==AttemptMilestone::RxArrival) {
        require(!a.rx_arrival,"statistics: duplicate RX arrival");(void)elapsed(e.at,a.handoff);a.rx_arrival=e.at;
    } else {
        require(a.rx_arrival && !a.accepted,"statistics: invalid acceptance");
        (void)elapsed(e.at,*a.rx_arrival);a.accepted=e.at;
    }
    note(e.at);
}
void StatisticsCollector::record(const StatAttemptDelivered& e) {
    auto& a=attempts_.at(e.id);
    require(a.accepted && !a.delivered && e.op==a.command.op,"statistics: invalid delivery");
    (void)elapsed(e.at,*a.accepted);a.delivered=e.at;a.status=e.status;note(e.at);
}
void StatisticsCollector::record(const StatAttemptAssessed& e) {
    auto& a=attempts_.at(e.id);require(a.delivered && !a.assessed,"statistics: invalid Host assessment");
    (void)elapsed(e.at,*a.delivered);a.assessed=e.at;a.host_success=e.success;note(e.at);
}
StatisticsSummary StatisticsCollector::summarize(Tick at) const {
    require(!(at<latest_),"statistics: snapshot predates observed events");
    StatisticsSummary s;s.at=at;
    for(auto op:{Op::Read,Op::Write}) {s.globals[op];s.logical_work[op];s.attempts[op];}
    std::map<std::string,std::vector<std::uint64_t>> samples;
    std::map<LogicalWorkId,Tick> first_handoff;
    for(const auto& entry:attempts_) {
        const auto& a=entry.second;const auto op=a.command.op;
        const auto result=!a.status?LogicalResult::Pending:*a.status==0?LogicalResult::Success:LogicalResult::Failed;
        count(s.attempts[op],result);s.replay_attempts+=a.replay;s.retried_unit_attempts+=a.unit_attempt>1;
        if(a.status)++s.responses[{op,*a.status}];
        if(a.status && *a.status==0 && a.host_success && !*a.host_success)++s.host_verification_failures;
        for(auto id:a.work) {auto it=first_handoff.find(id);if(it==first_handoff.end()||a.handoff<it->second)first_handoff[id]=a.handoff;}
        const std::string prefix=std::string(op_name(op))+".attempt."+outcome(result)+".";
        const auto add=[&](const char* metric,std::optional<Tick> end,std::optional<Tick> start) {
            if(end&&start)samples[prefix+metric].push_back(elapsed(*end,*start));
        };
        add("queue_wait",a.handoff,a.queued_at);add("request_link",a.rx_arrival,a.handoff);
        add("ingress_wait",a.accepted,a.rx_arrival);add("accepted_response",a.delivered,a.accepted);
        add("handoff_response",a.delivered,a.handoff);
    }
    for(const auto& entry:globals_) {
        const auto& g=entry.second;count(s.globals[g.op],g.result,g.finished&&!g.released);
        if(g.finished&&g.released)samples[std::string(op_name(g.op))+".global."+outcome(g.result)+".end_to_end"].push_back(elapsed(*g.finished,*g.released));
    }
    for(const auto& entry:work_) {
        const auto& w=entry.second;const auto& g=globals_.at(w.parent);
        count(s.logical_work[g.op],w.result,w.cancelled_before_release);
        const std::string prefix=std::string(op_name(g.op))+".logical."+outcome(w.result)+".";
        if(w.finished&&g.released&&!w.cancelled_before_release)
            samples[prefix+"end_to_end"].push_back(elapsed(*w.finished,*g.released));
        const auto handoff=first_handoff.find(w.id);
        if(handoff!=first_handoff.end()&&g.released&&!w.cancelled_before_release)
            samples[prefix+"initial_host_wait"].push_back(elapsed(handoff->second,*g.released));
    }
    for(auto& entry:samples)s.latency.emplace(entry.first,summarize_samples(entry.second));
    return s;
}
void StatisticsCollector::write_json(std::ostream& destination,Tick at,const std::string& stop) const {
    const auto s=summarize(at);
    // Keep serialization independent of the caller's locale/format flags.
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
    out<<"{\"schema_version\":1,\"time_unit\":\"tick\",\"at\":"<<at.value<<",\"stop_reason\":"<<quote(stop)
       <<",\"summary\":{\"globals\":";counts_json(out,s.globals);
    out<<",\"logical_work\":";counts_json(out,s.logical_work);out<<",\"attempts\":";counts_json(out,s.attempts);
    out<<",\"replay_attempts\":"<<s.replay_attempts<<",\"retried_unit_attempts\":"<<s.retried_unit_attempts
       <<",\"host_verification_failures\":"<<s.host_verification_failures<<",\"responses\":[";
    bool comma=false;
    for(const auto& entry:s.responses) {
        if(comma)out<<',';comma=true;
        out<<"{\"op\":"<<quote(op_name(entry.first.first))<<",\"status\":"<<unsigned(entry.first.second)<<",\"count\":"<<entry.second<<'}';
    }
    out<<"],\"latency\":{";comma=false;
    for(const auto& entry:s.latency) {
        if(comma)out<<',';comma=true;const auto& l=entry.second;
        out<<quote(entry.first)<<":{\"count\":"<<l.count<<",\"min\":"<<l.min<<",\"max\":"<<l.max<<",\"mean\":"<<l.mean
           <<",\"p50\":"<<l.p50<<",\"p95\":"<<l.p95<<",\"p99\":"<<l.p99<<'}';
    }
    out<<"}},\"globals\":[";comma=false;
    for(const auto& entry:globals_) {
        if(comma)out<<',';comma=true;const auto& g=entry.second;
        out<<"{\"id\":"<<g.id.value<<",\"op\":"<<quote(op_name(g.op))<<",\"bytes\":"<<g.bytes
           <<",\"scheduled_release\":"<<g.scheduled_release.value<<",\"released\":";time_json(out,g.released);
        out<<",\"finished\":";time_json(out,g.finished);
        out<<",\"result\":"<<quote(outcome(g.result))<<",\"remaining\":"<<g.remaining<<",\"reason\":"<<quote(g.reason)<<'}';
    }
    out<<"],\"logical_work\":[";comma=false;
    for(const auto& entry:work_) {
        if(comma)out<<',';comma=true;const auto& w=entry.second;
        out<<"{\"id\":"<<w.id.value<<",\"global_id\":"<<w.parent.value<<",\"channel\":"<<w.address.channel
           <<",\"local_byte\":"<<w.address.local.value<<",\"content\":";
        if(w.content)out<<*w.content;else out<<"null";
        out<<",\"cancelled_before_release\":"<<(w.cancelled_before_release?"true":"false")<<",\"released\":";
        time_json(out,w.cancelled_before_release?std::nullopt:globals_.at(w.parent).released);
        out<<",\"finished\":";time_json(out,w.finished);
        out<<",\"result\":"<<quote(outcome(w.result))<<",\"reason\":"<<quote(w.reason)<<'}';
    }
    out<<"],\"attempts\":[";comma=false;
    for(const auto& entry:attempts_) {
        if(comma)out<<',';comma=true;const auto& a=entry.second;
        out<<"{\"id\":"<<a.id.value<<",\"command\":"<<a.command.id.value<<",\"op\":"<<quote(op_name(a.command.op))
           <<",\"channel\":"<<a.command.address.channel<<",\"local_byte\":"<<a.command.address.local.value
           <<",\"bytes\":"<<a.command.size_bytes<<",\"unit\":";
        if(a.unit)out<<a.unit->value;else out<<"null";
        out<<",\"unit_attempt\":"<<a.unit_attempt<<",\"replay\":"<<(a.replay?"true":"false")<<",\"replay_pass\":"<<a.replay_pass
           <<",\"queued_at\":";time_json(out,a.queued_at);
        out<<",\"handoff\":"<<a.handoff.value<<",\"rx_arrival\":";time_json(out,a.rx_arrival);
        out<<",\"accepted\":";time_json(out,a.accepted);out<<",\"delivered\":";time_json(out,a.delivered);
        out<<",\"assessed\":";time_json(out,a.assessed);out<<",\"status\":";
        if(a.status)out<<unsigned(*a.status);else out<<"null";
        out<<",\"host_success\":";if(a.host_success)out<<(*a.host_success?"true":"false");else out<<"null";
        out<<",\"work_ids\":[";bool sep=false;for(auto id:a.work){if(sep)out<<',';sep=true;out<<id.value;}out<<"]}";
    }
    out<<"]}\n";destination<<out.str();
    if(!destination)throw std::runtime_error("statistics: JSON output failed");
}
} // namespace hbf_eventlab
