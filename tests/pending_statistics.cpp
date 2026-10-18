#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <cmath>
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::logic_error&){caught=true;}check(caught,"expected diagnostic");}
StaticDeviceDescription profile(unsigned channels=1,unsigned pending=2,Tick timeout=Tick{100}) {
    Geometry g{channels,1,1,8,4,1};return {g,{256,pending,false,true},{256,timeout,g.block_group_dlus()}};
}
struct AssemblyFixture {
    StaticDeviceDescription description; StatisticsCollector stats;Tick now{};
    RetainedAssemblyStorage storage;WriteAssembly assembly;std::uint64_t serial=0;
    AssemblyFixture(unsigned channels=1,unsigned limit=2):description(profile(channels,limit)),storage(channels*limit),assembly(description,storage) {
        for(ChannelId ch=0;ch<channels;++ch)stats.observe(StatPendingInitialized{ch,limit,now});
        assembly.set_observer([this](const AssemblyObservation& e) {
            stats.observe(StatDluObserved{now,e});
            const auto p=stats.summarize(now).pending_channels.at(e.key.channel);
            check(p.current.total==assembly.pending(e.key.channel),"observation follows actual committed count");
            const auto* ctx=assembly.find(e.key);
            check(e.change==AssemblyChange::Retired?!ctx:ctx&&ctx->received.count()==e.received_fragments,
                  "observation reads authoritative assembly state");
        });
    }
    AssemblyResult fragment(DluKey key,unsigned slot,CommandId id={}) {
        if(!id.value)id=CommandId{++serial};
        return assembly.accept({id,key,slot,0,1},DataToken{slot+1},now);
    }
    Generation fill(DluKey key,unsigned first=0,CommandId burst={}) {
        AssemblyResult r{};
        for(unsigned slot=first;slot<64;++slot) {
            if(burst.value)r=assembly.accept({burst,key,slot,slot,64},DataToken{slot+1},now);
            else r=fragment(key,slot);
            check(r.status==WriteStatus::Success,"fragment accepted");
        }
        return r.generation;
    }
    void start(DluKey k,Generation gen){assembly.queue_program(k,gen);assembly.start_program(k,gen);}
    void finish(DluKey k,Generation gen){assembly.commit_program(k,gen,[](const PageData&){});}
    const DluTiming& dlu(DluKey k,Generation gen) const{return stats.dlu_contexts().at({k.channel,k.dlu.value,gen.value});}
    PendingSummary summary(ChannelId ch=0) const{return stats.summarize(now).pending_channels.at(ch);}
};
const DluKey a{0,LocalDlu{0}},b{0,LocalDlu{8}};
void lifecycle() {
    AssemblyFixture f;f.now=Tick{10};auto gen=f.fragment(a,0).generation;
    check(f.summary().current.collecting==1&&f.dlu(a,gen).received_fragments==1,"one collecting fragment");
    f.now=Tick{20};f.fill(a,1);check(f.summary().current.ready_waiting==1,"complete means ready, not programming");
    f.now=Tick{30};f.assembly.queue_program(a,gen);
    check(f.summary().current.ready_waiting==1&&!f.dlu(a,gen).program_started_at,"queued still ready waiting");
    f.now=Tick{40};f.assembly.start_program(a,gen);f.now=Tick{60};f.finish(a,gen);
    const auto& d=f.dlu(a,gen);
    check(d.opened_at==Tick{10}&&d.ready_at==Tick{20}&&d.program_started_at==Tick{40}&&d.retired_at==Tick{60},"real milestone times");
    check(d.termination==DluTermination::ProgramSuccess&&d.members.size()==64&&d.progress.size()==67,"one lifecycle, 64 command members");
    const auto s=f.stats.summarize(f.now);
    check(s.latency.at("dlu_context.program_success.assembly").min==10&&
          s.latency.at("dlu_context.program_success.ready_wait").min==20&&
          s.latency.at("dlu_context.program_success.program").min==20,"phase durations");
    check(f.storage.used()==0&&f.summary().current.total==0,"storage and Pending released together");
}
void occupancy() {
    AssemblyFixture f(2);f.now=Tick{10};auto ga=f.fragment(a,0).generation;
    f.now=Tick{20};auto gb=f.fragment(b,0).generation;
    f.now=Tick{30};f.fill(a,1);f.now=Tick{40};f.assembly.queue_program(a,ga);
    f.now=Tick{50};f.assembly.start_program(a,ga);f.now=Tick{60};f.finish(a,ga);
    f.now=Tick{120};f.assembly.expire(b,gb,f.now);f.now=Tick{150};const auto s=f.summary();
    check(s.area.total==150&&s.area.collecting==120&&s.area.ready_waiting==20&&s.area.programming==10,"exact context-tick integral");
    check(s.mean.total==1&&s.at_limit_ticks==40&&s.opened==2&&s.retired==2,"mean and full interval");
    check(s.peak.total==2&&s.peak.collecting==2&&s.peak.ready_waiting==1&&s.peak.programming==1,"dimension-wise peaks");
    check(f.summary(1).area.total==0&&f.summary(1).opened==0,"untouched channel remains zero");
}
void rejected() {
    AssemblyFixture f(1,1);auto ga=f.fragment(a,0).generation;
    const auto history=f.stats.pending_timeline().size();
    check(f.fragment(a,0).status==WriteStatus::OverlapAddress&&f.fragment(b,0).status==WriteStatus::PendingDluLimit,"real rejections");
    check(f.stats.dlu_contexts().size()==1&&f.dlu(a,ga).members.size()==1&&
          f.dlu(a,ga).received_fragments==1&&f.stats.pending_timeline().size()==history,"reject creates no member, progress, or context");
}
void timeout_reuse() {
    AssemblyFixture f;f.now=Tick{10};auto old=f.fragment(a,0).generation;
    f.now=Tick{110};check(f.assembly.expire(a,old,f.now).has_value(),"timeout retired");
    const auto& d=f.dlu(a,old);
    check(d.termination==DluTermination::AssemblyTimeout&&!d.ready_at&&!d.program_started_at&&d.received_fragments==1,"partial timeout preserves missing milestones");
    auto fresh=f.fragment(a,0).generation;
    check(old!=fresh&&!f.assembly.expire(a,old,f.now)&&f.summary().current.total==1,"stale deadline cannot retire new context");
    f.now=Tick{120};f.fill(a,1);f.start(a,fresh);f.now=Tick{130};f.finish(a,fresh);
    check(f.stats.dlu_contexts().size()==2&&f.summary().opened==2&&f.summary().retired==2,"retry is a distinct assembly attempt");
}
void failure() {
    AssemblyFixture f(1,4);auto root=f.fill(a);auto ready=f.fill(b);
    const DluKey c{0,LocalDlu{16}};auto collecting=f.fragment(c,0).generation;f.start(a,root);
    f.now=Tick{10};f.assembly.fail_program(a,root);f.assembly.fail_prior_program(b,ready);f.assembly.fail_prior_program(c,collecting);
    check(f.dlu(a,root).termination==DluTermination::ProgramFail&&f.dlu(a,root).program_started_at==Tick{},"failed program keeps actual start");
    for(auto k:{b,c}) {const auto& d=f.dlu(k,k.dlu==b.dlu?ready:collecting);
        check(d.termination==DluTermination::PriorProgramFail&&d.additional_info==2&&!d.program_started_at,"followers distinct from failed media operation");}
    check(f.summary().current.total==0&&f.summary().retired==3,"fanout drains all phases");
}
void same_tick_burst() {
    AssemblyFixture f;auto gen=f.fill(a,0,CommandId{7});f.start(a,gen);f.finish(a,gen);
    const auto& d=f.dlu(a,gen);std::uint64_t seq=0;
    for(const auto& p:d.progress){check(p.at==Tick{}&&p.sequence>seq,"same Tick deterministic sequence");seq=p.sequence;}
    check(d.members.size()==1&&d.received_fragments==64&&d.ready_at==Tick{}&&d.retired_at==Tick{},"burst one command, 64 unique fragments; zero is valid time");
    check(f.summary().area.total==0&&f.summary().mean.total==0&&f.summary().peak.total==1,"zero-duration occupancy not invented");
    check(f.stats.pending_timeline().size()==5,"only count changes, no redundant samples for fragments/queue transition");
}
void commit_failure() {
    AssemblyFixture f;auto gen=f.fill(a);f.start(a,gen);f.now=Tick{10};
    rejects([&]{f.assembly.commit_program(a,gen,[](const PageData&){throw std::logic_error("media not committed");});});
    check(f.summary().current.programming==1&&!f.dlu(a,gen).retired_at&&f.storage.used()==1,"failed commit does not emit retirement");
    f.now=Tick{20};f.finish(a,gen);check(f.summary().retired==1,"later successful commit retires once");
}
void snapshot() {
    AssemblyFixture f;f.now=Tick{10};auto gen=f.fragment(a,0).generation;
    f.now=Tick{20};f.fill(a,1);f.now=Tick{30};f.start(a,gen);
    const auto n=f.stats.pending_timeline().size();
    check(f.stats.summarize(Tick{40}).pending_channels.at(0).area.total==30&&
          f.stats.summarize(Tick{100}).pending_channels.at(0).area.total==90,"active tail integrated through snapshot");
    check(f.stats.pending_timeline().size()==n&&!f.dlu(a,gen).retired_at,"snapshot does not mutate timeline or finalize active context");
    std::ostringstream json;f.stats.write_json(json,Tick{40},"LimitReached");
    check(json.str().find("\"retired_at\":null")!=std::string::npos&&json.str().find("\"observed_residence_ticks\":30")!=std::string::npos,"partial JSON retains censoring");
    f.now=Tick{50};f.finish(a,gen);f.now=Tick{60};check(f.summary().area.total==40,"snapshot does not bias resumed integral");
}
void teardown() {
    StatisticsCollector stats;RetainedAssemblyStorage storage(2);auto desc=profile();
    stats.observe(StatPendingInitialized{0,2,Tick{}});
    {WriteAssembly assembly(desc,storage);assembly.set_observer([&](const auto& e){stats.observe(StatDluObserved{Tick{},e});});
     assembly.accept({CommandId{1},a,0,0,1},DataToken{1},Tick{});}
    check(storage.used()==0&&stats.summarize(Tick{10}).pending_channels.at(0).current.collecting==1&&
          !stats.dlu_contexts().begin()->second.retired_at,"destructor frees memory but does not fake simulated completion");
}
void identity() {
    AssemblyFixture f;
    auto begin=[&](std::uint64_t id) {
        f.stats.observe(StatAttemptStarted{TransferAttemptId{id},{CommandId{7},Op::Write,{0,LocalByteAddress{}},4096},f.now});
        f.stats.observe(StatAttemptAdvanced{TransferAttemptId{id},AttemptMilestone::RxArrival,f.now});
        f.stats.observe(StatAttemptAdvanced{TransferAttemptId{id},AttemptMilestone::Accepted,f.now});
    };
    begin(1);auto first=f.fill(a,0,CommandId{7});f.start(a,first);f.now=Tick{10};f.finish(a,first);
    f.stats.observe(StatAttemptDelivered{TransferAttemptId{1},f.now,Op::Write,0});
    f.now=Tick{20};begin(2);auto second=f.fill(a,0,CommandId{7});
    check(f.dlu(a,first).members.at(CommandId{7})==TransferAttemptId{1}&&
          f.dlu(a,second).members.at(CommandId{7})==TransferAttemptId{2},"captured attempt not rebound when CommandId reused");
}
void guarded_attach() {
    AssemblyFixture f;rejects([&]{f.assembly.set_observer([](const auto&){});});
    RetainedAssemblyStorage storage(2);WriteAssembly assembly(f.description,storage);
    assembly.accept({CommandId{1},a,0,0,1},DataToken{},Tick{});
    rejects([&]{assembly.set_observer([](const auto&){});});
}
LocalTransaction burst(){return {{CommandId{1},Op::Write,{0,LocalByteAddress{}},4096},std::vector<DataToken>(64,DataToken{1})};}
void device_delivery() {
    StatisticsCollector stats;EventQueue events;DeviceDriver device(profile(1,2,Tick{10000000}),{},events);
    device.set_statistics_sink(stats);bool delivered=false;
    device.try_submit(burst(),[&](const auto&){delivered=true;});
    while(stats.dlu_contexts().empty()||!stats.dlu_contexts().begin()->second.retired_at)check(events.run_next(),"retirement event exists");
    const auto& ctx=stats.dlu_contexts().begin()->second;
    check(!delivered&&device.activity().outstanding==1&&stats.summarize(events.now()).pending_channels.at(0).current.total==0,"Pending freed while response and MOCS remain outstanding");
    events.run();const auto& attempt=stats.attempts().at(*ctx.members.begin()->second);
    check(delivered&&*ctx.retired_at<*attempt.delivered&&device.activity().drained(),"response later delivered and credit drained");
}
void device_limit() {
    StatisticsCollector stats;EventQueue events;DeviceDriver device(profile(1,2,Tick{10000000}),{},events);
    RoundRobinInterleaver inter({},1,device.description().geometry().channel_bytes());HostDriver host(device,events,inter,{});
    SimulationRunner runner(events,host,device);device.set_statistics_sink(stats);host.set_statistics_sink(stats);
    host.load({{GlobalRequestId{1},Op::Write,GlobalByteAddress{},4096,1}});host.start();
    RunLimits limits;limits.max_tick=Tick{10000};const auto partial=runner.run(limits);
    check(partial.status==RunStatus::LimitReached&&stats.dlu_contexts().size()==1,"stop during real assembly");
    const auto& ctx=stats.dlu_contexts().begin()->second;
    check(ctx.received_fragments>0&&ctx.received_fragments<64&&!ctx.retired_at,"unfinished partial occupancy retained");
    const auto count=stats.pending_timeline().size();stats.summarize(partial.at);stats.summarize(partial.at);
    check(stats.pending_timeline().size()==count,"repeated summary no extra samples");
    const auto final=runner.run();check(final.status==RunStatus::Completed&&final.workload_success&&
        stats.dlu_contexts().size()==1&&stats.summarize(final.at).pending_channels.at(0).retired==1,"resume same context finishes once");
}
void read_only() {
    StatisticsCollector stats;EventQueue events;DeviceDriver device(profile(),{},events);device.set_statistics_sink(stats);
    device.try_submit({{CommandId{1},Op::Read,{0,LocalByteAddress{}},64},{}},[](const auto&){});events.run();
    const auto s=stats.summarize(events.now());
    check(stats.dlu_contexts().empty()&&stats.pending_timeline().size()==1&&s.pending_channels.at(0).area.total==0,"read errors still have zero Pending curve");
}
void curve_reconstruction() {
    AssemblyFixture f(2);f.now=Tick{10};auto ga=f.fragment(a,0).generation;
    const DluKey other{1,LocalDlu{0}};f.now=Tick{12};auto go=f.fill(other,0,CommandId{1000});f.start(other,go);
    f.now=Tick{20};f.fill(a,1);f.start(a,ga);f.now=Tick{30};f.finish(other,go);f.now=Tick{50};f.finish(a,ga);f.now=Tick{70};
    for(ChannelId ch=0;ch<2;++ch) {
        PendingIntegrals area;PendingCounts previous;Tick last{};std::uint64_t seq=0;
        auto integrate=[&](Tick at){auto dt=at.value-last.value;area.total+=previous.total*dt;area.collecting+=previous.collecting*dt;
            area.ready_waiting+=previous.ready_waiting*dt;area.programming+=previous.programming*dt;last=at;};
        for(const auto& point:f.stats.pending_timeline())if(point.channel==ch){
            check(point.sequence>seq&&!(point.at<last),"ordered channel samples");seq=point.sequence;integrate(point.at);
            previous=point.counts;check(previous.total==previous.collecting+previous.ready_waiting+previous.programming,"curve partition");}
        integrate(f.now);const auto expected=f.summary(ch).area;
        check(area.total==expected.total&&area.collecting==expected.collecting&&area.ready_waiting==expected.ready_waiting&&area.programming==expected.programming,"public timeline reproduces all integrals including final tail");
    }
}
}
int main(int argc,char** argv) {
    const std::map<std::string,void(*)()> tests{{"lifecycle",lifecycle},{"occupancy",occupancy},{"rejected",rejected},
        {"timeout_reuse",timeout_reuse},{"failure",failure},{"same_tick_burst",same_tick_burst},{"commit_failure",commit_failure},
        {"snapshot",snapshot},{"teardown",teardown},{"identity",identity},{"guarded_attach",guarded_attach},
        {"device_delivery",device_delivery},{"device_limit",device_limit},{"read_only",read_only},{"curve_reconstruction",curve_reconstruction}};
    try{if(argc!=2||!tests.count(argv[1]))throw std::invalid_argument("unknown test");tests.at(argv[1])();
        std::cout<<"PASS "<<argv[1]<<" ("<<checks<<" checks)\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<(argc>1?argv[1]:"")<<": "<<e.what()<<'\n';return 1;}
}
