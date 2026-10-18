#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool value,const char* why) {++checks;if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F f) {bool bad=false;try{f();}catch(const std::logic_error&){bad=true;}check(bad,"invalid statistics usage rejected");}
StaticDeviceDescription profile(unsigned channels=1,unsigned pending=8,Tick deadline=Tick{10000000},unsigned mocs=256) {
    Geometry g{channels,1,1,8,4,1};return {g,{256,pending,false,true},{mocs,deadline,g.block_group_dlus()}};
}
GlobalRequest wr(std::uint64_t id,std::uint64_t a,std::uint64_t bytes=4096,Tick release={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{a},bytes,1};r.not_before=release;return r;
}
GlobalRequest rd(std::uint64_t id,std::uint64_t a,std::uint64_t bytes=64,std::vector<GlobalRequestId> after={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{a},bytes};r.after=std::move(after);return r;
}
struct Fixture {
    StatisticsCollector stats;EventQueue events;DeviceDriver device;RoundRobinInterleaver inter;
    HostDriver host;SimulationRunner runner;
    Fixture(HostConfiguration hc={},ExecutionConfiguration ec={},unsigned channels=1,unsigned pending=8,
            Tick deadline=Tick{10000000},unsigned mocs=256,bool enabled=true)
      :device(profile(channels,pending,deadline,mocs),ec,events),
       inter({},channels,device.description().geometry().channel_bytes()),host(device,events,inter,hc),runner(events,host,device) {
        if(enabled){device.set_statistics_sink(stats);host.set_statistics_sink(stats);}
    }
    RunReport run(const std::vector<GlobalRequest>& input,bool success=true) {
        host.load(input);host.start();auto r=runner.run();
        if(r.status!=RunStatus::Completed)std::cerr<<r.explanation<<'\n';
        check(r.status==RunStatus::Completed && r.workload_success==success,"closed loop expected outcome");
        check(r.host.drained()&&r.device.drained(),"closed loop drained");return r;
    }
};
void boundaries() {
    HostConfiguration hc;hc.writes=WriteCommandMode::Burst4096;
    Fixture f(hc);auto r=f.run({wr(1,0,4096,Tick{1000}),rd(2,0,64,{GlobalRequestId{1}})});
    const auto& a=f.stats.attempts().begin()->second;
    const auto& ec=f.device.configuration();
    check(a.handoff==Tick{1000} && f.stats.globals().at(GlobalRequestId{1}).released==Tick{1000},"release excludes initial idle time");
    check(a.rx_arrival->value-a.handoff.value==ec.ucie.serialization(4112).value+ec.ucie.overhead.request_fixed_latency.value,"request serialization plus propagation");
    check(a.accepted==a.rx_arrival,"available ingress accepted at arrival");
    check(a.delivered.has_value() && a.assessed==a.delivered,"delivery and later-round Host feedback share Tick");
    const auto s=f.stats.summarize(r.at);
    const auto& latency=s.latency.at("write.attempt.success.accepted_response");
    check(latency.count==1 && latency.min==a.delivered->value-a.accepted->value,"burst counts one accepted response");
    check(s.logical_work.at(Op::Write).success==64 && s.attempts.at(Op::Write).success==1,"logical and attempt units differ");
    const auto& g=f.stats.globals().at(GlobalRequestId{1});
    check(g.finished==a.delivered && !g.remaining,"Global complete after all children");
    check(s.latency.at("write.logical.success.end_to_end").count==64,"fragment end-to-end sample count");
    check(s.latency.at("read.logical.success.initial_host_wait").min>=g.finished->value,"dependency wait included in Host wait");
}
void cross_global_burst() {
    HostConfiguration hc;hc.writes=WriteCommandMode::Burst4096;Fixture f(hc);
    auto r=f.run({wr(1,0,2048),wr(2,2048,2048,Tick{20000})});
    check(f.stats.attempts().size()==1,"one command shared by two Globals");
    const auto& a=f.stats.attempts().begin()->second;
    check(a.work.size()==64 && a.handoff==Tick{20000},"shared burst waits for last contributor release");
    const auto& g1=f.stats.globals().at(GlobalRequestId{1});const auto& g2=f.stats.globals().at(GlobalRequestId{2});
    check(g1.finished==g2.finished && g1.released==Tick{} && g2.released==Tick{20000},"distinct release same shared completion");
    const auto s=f.stats.summarize(r.at);
    const auto& l=s.latency.at("write.global.success.end_to_end");
    check(l.count==2 && l.max-l.min==20000,"Global latency uses own release");
    check(s.responses.at({Op::Write,0})==1,"shared response not duplicated");
    const auto& context=f.stats.dlu_contexts().begin()->second;
    check(f.stats.dlu_contexts().size()==1&&context.members.size()==1&&
          context.members.begin()->second==a.id,"one assembly context links shared burst attempt and both Globals");
}
void multi_channel() {
    Fixture f({}, {},2);auto r=f.run({wr(1,0,8192)});
    const auto& g=f.stats.globals().at(GlobalRequestId{1});Tick latest{},earliest{UINT64_MAX};std::set<ChannelId> channels;
    for(const auto& entry:f.stats.logical_work()) {
        const auto& w=entry.second;channels.insert(w.address.channel);
        if(latest<*w.finished)latest=*w.finished;if(*w.finished<earliest)earliest=*w.finished;
    }
    check(channels.size()==2 && earliest<latest && g.finished==latest,"Global uses latest logical result across channels");
    check(f.stats.summarize(r.at).logical_work.at(Op::Write).success==128,"original total counted once");
}
void pending_retry() {
    HostConfiguration hc;hc.max_open_dlus_per_channel=3;hc.pending_retry_delay=Tick{10000000};
    ExecutionConfiguration ec;ec.media_timing.program=Tick{3000000};Fixture f(hc,ec,1,1);
    auto r=f.run({wr(1,0),wr(2,32768)});auto s=f.stats.summarize(r.at);
    check(s.responses.at({Op::Write,4})>0 && s.retried_unit_attempts>0,"Pending errors and retries separate");
    check(s.logical_work.at(Op::Write).success==128 && s.logical_work.at(Op::Write).failed==0,"failed attempts can finish logically successful");
    bool retry=false;
    for(const auto& entry:f.stats.attempts()) {
        const auto& a=entry.second;
        if(a.unit_attempt>1) {retry=true;check(a.queued_at->value>0,"retry enqueue time renewed");
            for(auto id:a.work)check(f.stats.globals().at(f.stats.logical_work().at(id).parent).released==Tick{},"logical release never reset");}
        check(a.accepted && a.delivered && a.assessed,"even error commands have formal acceptance and delivery");
    }
    check(retry,"real retry observed");
    check(s.latency.at("write.attempt.failed.accepted_response").count==s.attempts.at(Op::Write).failed,"error latencies not mixed into success");
    check(f.stats.dlu_contexts().size()==2,"0x4 rejections create no extra Pending contexts");
    for(const auto& entry:f.stats.dlu_contexts())for(const auto& member:entry.second.members)
        check(f.stats.attempts().at(*member.second).status==0,"rejected attempt excluded from assembly membership");
}
void timeout_retry() {
    Fixture f({}, {},1,8,Tick{300000});
    auto r=f.run({wr(1,0,1024),wr(2,1024,3072,Tick{1000000})});auto s=f.stats.summarize(r.at);
    check(s.responses.at({Op::Write,5})>0 && s.retried_unit_attempts>0,"timeout attempts included");
    check(s.logical_work.at(Op::Write).success==64 && s.globals.at(Op::Write).success==2,"whole DLU retry does not duplicate work");
    check(f.stats.globals().at(GlobalRequestId{1}).released==Tick{} && f.stats.globals().at(GlobalRequestId{2}).released==Tick{1000000},"later original fragment retains original release");
    std::size_t expired=0,succeeded=0;std::set<LogicalWorkId> timeout_work,retry_work;
    for(const auto& entry:f.stats.dlu_contexts()) {
        const auto& d=entry.second;const bool timeout=d.termination==DluTermination::AssemblyTimeout;
        expired+=timeout;succeeded+=d.termination==DluTermination::ProgramSuccess;
        for(const auto& member:d.members)for(auto id:f.stats.attempts().at(*member.second).work)
            (timeout?timeout_work:retry_work).insert(id);
    }
    check(expired>0&&succeeded==1&&!timeout_work.empty(),"real timeout has separate retired and successful context records");
    for(auto id:timeout_work)check(retry_work.count(id)==1,"new context retry still links original logical work");
}
void replay() {
    Fixture f;f.device.set_program_outcome([step=0](const auto& op)mutable {
        if((step==0&&op.physical.page==1)||(step==1&&op.physical.page==0)){++step;return ProgramOutcome::FailedReusable;}
        return ProgramOutcome::Success;
    });
    auto r=f.run({wr(1,0,8192)});const auto s=f.stats.summarize(r.at);
    check(s.responses.at({Op::Write,7})>0 && s.replay_attempts>0,"program failures and replay measured");
    check(f.host.statistics().replay_passes==2 && s.logical_work.at(Op::Write).success==128,"merged recovery retains original work count");
    std::set<std::uint64_t> passes;
    for(const auto& entry:f.stats.attempts())if(entry.second.replay) {
        const auto& a=entry.second;passes.insert(a.replay_pass);
        check(a.work.size()==a.command.size_bytes/64,"replay associated to full original footprint");
        for(auto id:a.work)check(f.stats.logical_work().count(id)==1 && id.value!=0,"replay has no fabricated zero-ID work");
    }
    check(passes.size()==2,"multiple passes remain one logical workload");
    check(s.latency.at("write.logical.success.end_to_end").count==128,"earlier successes not counted again by replay");
    std::size_t failed=0,replayed_contexts=0;
    for(const auto& entry:f.stats.dlu_contexts()) {
        const auto& d=entry.second;failed+=d.termination==DluTermination::ProgramFail;
        check(d.retired_at.has_value(),"recovery ends all contexts");
        bool replayed=false;for(const auto& member:d.members)replayed|=f.stats.attempts().at(*member.second).replay;
        replayed_contexts+=replayed;
    }
    check(failed==2&&replayed_contexts>0,"repeated program failures and replay contexts individually observable");
}
void unfinished_resume() {
    Fixture f;f.host.load({wr(1,0)});f.host.start();RunLimits limits;limits.max_tick=Tick{100};
    auto r=f.runner.run(limits);auto s=f.stats.summarize(r.at);
    check(r.status==RunStatus::LimitReached && s.globals.at(Op::Write).pending==1,"unfinished Global retained");
    check(s.attempts.at(Op::Write).pending==1 && !f.stats.attempts().begin()->second.rx_arrival,"link-only attempt retained");
    check(!s.latency.count("write.attempt.success.accepted_response")&&!s.latency.count("write.global.success.end_to_end"),"unfinished has no fabricated latency");
    r=f.runner.run();s=f.stats.summarize(r.at);
    check(r.workload_success && s.attempts.at(Op::Write).success==64 && s.globals.at(Op::Write).pending==0,"resume updates same records exactly once");
}
void ingress_wait() {
    HostConfiguration hc;hc.writes=WriteCommandMode::Burst4096;Fixture f(hc,{},1,8,Tick{10000000},1);
    auto r=f.run({wr(1,0),wr(2,32768)});bool blocked=false;
    for(const auto& entry:f.stats.attempts()) {const auto& a=entry.second;if(*a.rx_arrival<*a.accepted)blocked=true;}
    check(blocked && f.stats.summarize(r.at).latency.at("write.attempt.success.ingress_wait").max>0,"RX and MOCS acceptance are distinct");
}
void cancelled_future() {
    HostConfiguration hc;hc.max_timeout_retries=0;Fixture f(hc);
    auto read=rd(2,0,64,{GlobalRequestId{1}});read.not_before=Tick{2000000000000ULL};
    auto r=f.run({wr(1,0,64),read},false);const auto s=f.stats.summarize(r.at);
    const auto& g=f.stats.globals().at(GlobalRequestId{2});
    check(!g.released && g.finished && *g.finished<g.scheduled_release,"canceled-before-release explicit");
    check(s.globals.at(Op::Read).cancelled_before_release==1 && s.logical_work.at(Op::Read).cancelled_before_release==1,"canceled subset reported");
    check(!s.latency.count("read.global.failed.end_to_end"),"cancellation cannot create negative latency");
}
void mismatch() {
    Fixture f;auto read=rd(2,0,64,{GlobalRequestId{1}});read.expected_content=9;
    auto r=f.run({wr(1,0),read},false);auto s=f.stats.summarize(r.at);
    check(s.responses.at({Op::Read,0})==1 && s.attempts.at(Op::Read).success==1,"protocol result preserved");
    check(s.host_verification_failures==1 && s.logical_work.at(Op::Read).failed==1,"Host data mismatch separate");
}
void partial_future_cancel() {
    HostConfiguration hc;hc.max_timeout_retries=0;Fixture f(hc);
    // The later Global supplies the rest of block 0 AND page 0 of block 1.
    // Block 0 times out before that release; block 1 remains valid work.
    auto r=f.run({wr(1,0,64),wr(2,64,32768-64+4096,Tick{20000000})},false);
    const auto s=f.stats.summarize(r.at);const auto& g=f.stats.globals().at(GlobalRequestId{2});
    check(g.released==Tick{20000000} && g.finished && *g.released<*g.finished,"partially canceled Global still releases and completes remaining work");
    check(s.logical_work.at(Op::Write).cancelled_before_release==511 &&
          s.logical_work.at(Op::Write).success==64,"child cancellation not inferred from parent's final release state");
    check(s.latency.at("write.logical.failed.end_to_end").count==1,"only released failed fragment has a latency sample");
    check(s.latency.at("write.global.failed.end_to_end").count==2,"both released Globals have final failure times");
}
void backpressure() {
    StatisticsCollector stats;EventQueue events;DeviceDriver d(profile(),{},events);d.set_statistics_sink(stats);
    LocalTransaction tx{{CommandId{1},Op::Read,{0,LocalByteAddress{0}},64},{}};
    const auto first=d.try_submit(tx,[](const auto&){});tx.command.id=CommandId{2};
    const auto blocked=d.try_submit(tx,[](const auto&){});
    check(first.disposition==HandoffDisposition::Taken && blocked.disposition==HandoffDisposition::NotTaken,"link backpressure baseline");
    check(stats.attempts().size()==1,"NotTaken creates no physical attempt record");
    events.run();auto s=stats.summarize(events.now());
    check(s.attempts.at(Op::Read).failed==1 && s.responses.at({Op::Read,7})==1,"Device-only measurement supports standalone users");
}
void observational() {
    for(const auto& name:{"fcfs","fill_open"}) {
        HostConfiguration c;c.send_policy=name;
        Fixture on(c),off(c, {},1,8,Tick{10000000},256,false);
        auto read=rd(2,0,4096,{GlobalRequestId{1}});read.not_before=Tick{10000};
        const std::vector<GlobalRequest> input{wr(1,0,4096,Tick{5000}),read};
        const auto a=on.run(input),b=off.run(input);
        check(a.at==b.at && a.events==b.events,"statistics add no events or simulated latency");
        check(on.device.statistics().programs==off.device.statistics().programs &&
              on.device.statistics().cache_hits==off.device.statistics().cache_hits &&
              on.host.statistics().handed==off.host.statistics().handed,"no behavior changes with collection");
        check(off.stats.attempts().empty(),"disabled collection stores no history");
    }
}
void event_time() {
    StatisticsCollector s;
    s.observe(StatAttemptStarted{TransferAttemptId{1},{CommandId{1},Op::Read,{0,LocalByteAddress{}},64},Tick{10}});
    s.observe(StatAttemptAdvanced{TransferAttemptId{1},AttemptMilestone::RxArrival,Tick{15}});
    s.observe(StatAttemptAdvanced{TransferAttemptId{1},AttemptMilestone::Accepted,Tick{20}});
    s.observe(StatAttemptDelivered{TransferAttemptId{1},Tick{40},Op::Read,7});
    s.observe(StatAttemptAssessed{TransferAttemptId{1},Tick{100},false});
    const auto summary=s.summarize(Tick{1000});
    check(summary.latency.at("read.attempt.failed.accepted_response").mean==20,"delivery time not Host observation or snapshot time");
    check(summary.latency.at("read.attempt.failed.handoff_response").max==30,"transport time not callback execution time");
    rejects([&]{s.observe(StatAttemptAdvanced{TransferAttemptId{1},AttemptMilestone::Accepted,Tick{25}});});
    rejects([&]{s.summarize(Tick{99});});
}
void zero_and_status() {
    StatisticsCollector s;
    for(std::uint64_t i=1;i<=2;++i) {
        const auto op=i==1?Op::Read:Op::Write;const TransferAttemptId id{i};
        s.observe(StatAttemptStarted{id,{CommandId{i},op,{0,LocalByteAddress{}},64},Tick{}});
        s.observe(StatAttemptAdvanced{id,AttemptMilestone::RxArrival,Tick{}});
        s.observe(StatAttemptAdvanced{id,AttemptMilestone::Accepted,Tick{}});
        s.observe(StatAttemptDelivered{id,Tick{},op,2});
    }
    auto result=s.summarize(Tick{});
    check(result.responses.size()==2,"same numeric status separated by operation");
    check(result.latency.at("read.attempt.failed.accepted_response").count==1 &&
          result.latency.at("write.attempt.failed.accepted_response").max==0,"valid zero latency distinct from missing");
}
void guards_empty() {
    Fixture f;f.run({});auto summary=f.stats.summarize(f.events.now());
    check(summary.attempts.at(Op::Read).total==0 && summary.latency.empty(),"empty workload produces no artificial samples");
    rejects([&]{f.host.set_statistics_sink(f.stats);});rejects([&]{f.device.set_statistics_sink(f.stats);});
    std::ostringstream out;f.stats.write_json(out,f.events.now(),"a\"b\nc");
    check(out.str().find("a\\\"b\\nc")==std::string::npos && out.str().find("a\\\"b\\u000ac")!=std::string::npos,"JSON quotes and control characters escaped");
}
void stalled() {
    Fixture f;f.host.load({wr(1,4096)});f.host.start();auto r=f.runner.run();auto s=f.stats.summarize(r.at);
    check(r.status==RunStatus::Stalled && s.globals.at(Op::Write).pending==1 && s.attempts.at(Op::Write).total==0,"stalled logical work not silently lost");
    std::ostringstream out;f.stats.write_json(out,r.at,to_string(r.status));
    check(out.str().find("\"finished\":null")!=std::string::npos && out.str().find("Stalled")!=std::string::npos,"JSON retains missing times and stop reason");
}
} // namespace
int main(int argc,char** argv) {
    const std::map<std::string,void(*)()> tests{{"boundaries",boundaries},{"cross_global_burst",cross_global_burst},
        {"multi_channel",multi_channel},{"pending_retry",pending_retry},{"timeout_retry",timeout_retry},{"replay",replay},
        {"unfinished_resume",unfinished_resume},{"ingress_wait",ingress_wait},{"cancelled_future",cancelled_future},
        {"mismatch",mismatch},{"partial_future_cancel",partial_future_cancel},{"backpressure",backpressure},{"observational",observational},{"event_time",event_time},
        {"zero_and_status",zero_and_status},{"guards_empty",guards_empty},{"stalled",stalled}};
    try {if(argc!=2||!tests.count(argv[1]))throw std::invalid_argument("unknown test");
        tests.at(argv[1])();std::cout<<"PASS "<<argv[1]<<" ("<<checks<<" checks)\n";return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL "<<(argc>1?argv[1]:"")<<": "<<e.what()<<'\n';return 1;}
}
