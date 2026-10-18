#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/adapters/workload_reader.hpp"
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
template<class F> void rejects(F f,const std::string& text=""){
    bool yes=false;try{f();}catch(const std::exception&e){yes=text.empty()||std::string(e.what()).find(text)!=std::string::npos;}
    check(yes,"expected specific diagnostic");
}
StaticDeviceDescription profile(unsigned channels=1,unsigned mocs=256,Tick deadline=Tick{10000000}){
    Geometry g{channels,1,1,8,4,1};return {g,{256,8,false,true},{mocs,deadline,g.block_group_dlus()}};
}
GlobalRequest wr(std::uint64_t id,std::uint64_t a,std::uint64_t bytes=4096,std::uint64_t content=1){
    return {GlobalRequestId{id},Op::Write,GlobalByteAddress{a},bytes,content};
}
GlobalRequest rd(std::uint64_t id,std::uint64_t a,std::uint64_t bytes=64,std::vector<GlobalRequestId> after={},std::optional<std::uint64_t> expected=1){
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{a},bytes};r.after=std::move(after);r.expected_content=expected;return r;
}
class ReverseLink:public IUcieArbitrationPolicy{
public:std::uint64_t select(const std::vector<TransferCandidateView>& c)const override{return c.back().identity;}
};
struct Log {std::string action;Tick at;CommandId command;SendUnitId unit;LogicalBlockKey block;std::uint64_t pass;};
struct Fixture {
    EventQueue events;DeviceDriver device;RoundRobinInterleaver interleaver;HostDriver host;SimulationRunner runner;
    std::vector<Log> trace;
    Fixture(HostConfiguration hc={},ExecutionConfiguration ec={},unsigned ch=1,unsigned mocs=256,
            Tick deadline=Tick{10000000},std::unique_ptr<IUcieArbitrationPolicy> link=std::make_unique<FifoUciePolicy>(),
            std::unique_ptr<IHostSendPolicy> policy={})
      :device(profile(ch,mocs,deadline),ec,events,std::make_unique<FcfsNandPolicy>(),std::move(link)),
       interleaver(GlobalByteAddress{},ch,device.description().geometry().channel_bytes()),
       host(device,events,interleaver,hc,std::move(policy)),runner(events,host,device) {
        host.set_trace([this](const HostTrace&e){trace.push_back({e.action,e.at,e.command,e.unit,e.block,e.pass});});
    }
    RunReport run(std::vector<GlobalRequest> in,bool success=true){
        host.load(in);host.start();auto report=runner.run();
        if(report.status!=RunStatus::Completed)std::cerr<<to_string(report.status)<<": "<<report.explanation<<"\n";
        check(report.status==RunStatus::Completed,"closed loop completed");
        check(report.workload_success==success,"logical outcome");
        check(report.host.drained()&&report.device.drained(),"both sides drained");
        check(host.statistics().handed==host.statistics().responses,"every taken attempt finalized once");
        check(host.statistics().accepted==host.statistics().responses,"each attempt accepted and answered");
        check(device.statistics().request_transfers==host.statistics().handed,"waits do not retransmit");
        return report;
    }
    void fail_sequence(std::vector<PageId> pages,bool unusable=false){
        device.set_program_outcome([pages=std::move(pages),index=std::size_t{},unusable](const auto& op)mutable{
            if(op.logical.block==0&&index<pages.size()&&op.physical.page==pages[index]){
                ++index;return unusable?ProgramOutcome::FailedUnusable:ProgramOutcome::FailedReusable;
            }
            return ProgramOutcome::Success;
        });
    }
    const RecoveryTask& task(){return host.recovery().tasks().at({{0,0,0,0},1});}
};
void normal(){Fixture f;f.run({wr(1,0),rd(2,0,4096,{GlobalRequestId{1}})});
    check(f.device.statistics().programs==1&&f.host.statistics().responses==128,"one DLU / 128 commands");}
void sixteen_channels(){
    ExecutionConfiguration ec;ec.ucie.speed_grade=UcieSpeedGrade::Grade2;
    Fixture f({}, ec,16);std::vector<GlobalRequest> v;std::vector<GlobalRequestId> deps;
    for(unsigned n=0;n<16;++n){v.push_back(wr(n+1,n*4096));deps.push_back(GlobalRequestId{n+1});}
    v.push_back(rd(17,0,16*4096,deps));f.run(v);
    check(f.device.statistics().programs==16,"16 cross-entry DLUs");
}
void burst_compare(){
    Fixture a;a.run({wr(1,0),rd(2,0,4096,{GlobalRequestId{1}})});
    HostConfiguration c;c.writes=WriteCommandMode::Burst4096;Fixture b(c);b.run({wr(1,0),rd(2,0,4096,{GlobalRequestId{1}})});
    check(a.host.statistics().handed==128&&b.host.statistics().handed==65,"explicit burst changes command count only");
}
void burst_cross_global(){
    HostConfiguration c;c.writes=WriteCommandMode::Burst4096;Fixture f(c);
    f.run({wr(1,0,2048),wr(2,2048,2048),rd(3,0,4096,{GlobalRequestId{1},GlobalRequestId{2}})});
    check(f.host.statistics().handed==65,"burst can contain work from two Global entries");
}
void input_validation(){
    Fixture f;auto w=wr(1,0);w.after={GlobalRequestId{2}};
    rejects([&]{f.host.load({w,wr(2,4096)});},"LIMIT-WRITE-001");
    rejects([&]{f.host.load({wr(1,1)});},"unaligned");
    rejects([&]{f.host.load({wr(1,0),wr(1,4096)});},"duplicate Global");
    rejects([&]{f.host.load({rd(1,0,64,{GlobalRequestId{2}}),rd(2,0,64,{GlobalRequestId{1}})});},"cyclic");
    rejects([&]{f.host.load({rd(1,0,64,{GlobalRequestId{99}})});},"unknown");
    check(f.device.statistics().handoffs==0,"invalid workload has no external side effects");
}
void content_validation(){
    Fixture f;rejects([&]{f.host.load({wr(1,0,64),wr(2,0,64)});},"LIMIT-CONTENT-002");
    rejects([&]{f.host.load({wr(1,0,64,1),wr(2,0,64,2),wr(3,64,64,1)});},"LIMIT-CONTENT-001");
}
void burst_validation(){
    HostConfiguration c;c.writes=WriteCommandMode::Burst4096;Fixture f(c);
    rejects([&]{f.host.load({wr(1,0,64)});},"BURST-INCOMPLETE");
}
void splitter(){
    auto desc=profile(16);ReferenceAddressLayout layout(desc.geometry());
    RoundRobinInterleaver inter(GlobalByteAddress{8192},16,desc.geometry().channel_bytes(),128);
    GlobalRequestSplitter split(inter,layout);auto v=split.split(wr(1,8192,4096));
    check(v.size()==64,"64 fragments");
    check(v[0].address.channel==0&&v[1].address.channel==0&&v[2].address.channel==1,"128-byte stripe");
    check(v[32].address.local.value==128&&v[32].address.channel==0,"local stripe offset preserved");
    rejects([&]{split.split(wr(1,0,64));});
    rejects([&]{split.split(wr(1,~std::uint64_t{63},128));});
}
void parser(){
    std::istringstream in("HBF_EVENTLAB_WORKLOAD 1\n# comment\nWRITE 1 0x0 4096 content=9\nREAD 2 0 64 after=1 group=7 expect=9 at=2\n");
    auto v=read_workload(in);check(v.size()==2&&v[1].group==ReadGroup{7}&&v[1].not_before==Tick{2},"parsed optional fields");
    for(auto text:{"HBF_EVENTLAB_WORKLOAD 2\n","HBF_EVENTLAB_WORKLOAD 1\nWRITE 1 0 64\n",
        "HBF_EVENTLAB_WORKLOAD 1\nREAD 1 -1 64\n","HBF_EVENTLAB_WORKLOAD 1\nREAD 1 0 64 bogus=1\n",
        "HBF_EVENTLAB_WORKLOAD 1\nREAD 1 0 64 at=1 at=2\n"}){
        rejects([&]{std::istringstream bad(text);read_workload(bad);});
    }
}
void hints(){
    HostConfiguration c;c.hints=ReadHintMode::AllRegular;ExecutionConfiguration ec;ec.read_dispatch.threshold=0;ec.read_dispatch.wait=Tick{100000};
    Fixture a(c,ec);auto x=a.run({wr(1,0),rd(2,0,64,{GlobalRequestId{1}})});
    c.hints=ReadHintMode::TailBatch;Fixture b(c,ec);auto y=b.run({wr(1,0),rd(2,0,64,{GlobalRequestId{1}})});
    check(y.at<x.at,"tail B closes window earlier than timer");
}
void ordered_reads(){
    Fixture f;auto r=rd(2,0,8192,{GlobalRequestId{1}});r.group=ReadGroup{4};f.run({wr(1,0,8192),r});
    check(f.device.statistics().senses==2,"same group different pages supported");
}
void open_limit(){
    HostConfiguration c;c.max_open_dlus_per_channel=1;Fixture f(c);f.run({wr(1,0,4*4096)});
    check(f.device.statistics().programs==4,"one open DLU still progresses sequentially");
}
void fill_open(){
    HostConfiguration c;c.send_policy="fill_open";c.max_open_dlus_per_channel=1;Fixture f(c);
    f.run({wr(1,0,2048),wr(2,32768,4096,2),wr(3,2048,2048)});
    auto second=std::find_if(f.trace.begin(),f.trace.end(),[](auto&e){return e.action=="handoff"&&e.block.block==1;});
    std::size_t prior=0;for(auto it=f.trace.begin();it!=second;++it)prior+=it->action=="handoff"&&it->block.block==0;
    check(prior==64,"supplements bypass unrelated full DLU even across Global entries");
}
void recovery_once(){
    HostConfiguration c;c.max_open_dlus_per_channel=4;Fixture f(c);f.fail_sequence({2});
    f.run({wr(1,0,4*4096),rd(2,0,4*4096,{GlobalRequestId{1}})});
    check(f.host.statistics().recovery_tasks==1&&f.host.statistics().replay_passes==1,"one merged recovery");
    check(f.task().through==2&&f.task().phase==RecoveryPhase::Completed,"recovery endpoint");
    check(f.host.statistics().attempt_errors>=64,"64 error responses coalesced");
}
void follower_before_root(){
    HostConfiguration c;c.max_open_dlus_per_channel=4;ExecutionConfiguration ec;ec.buffers.egress={256,16384};Fixture f(c,ec,1,256,Tick{10000000},std::make_unique<ReverseLink>());
    f.fail_sequence({2});f.run({wr(1,0,4*4096),rd(2,0,4*4096,{GlobalRequestId{1}})});
    auto first=std::find_if(f.trace.begin(),f.trace.end(),[](auto&e){return e.action=="program-fail"||e.action=="follower-fail";});
    check(first!=f.trace.end()&&first->action=="follower-fail","follower arrived before root");
    check(f.task().through==2&&f.host.statistics().recovery_tasks==1,"0x6 not mistaken for failed page3");
}
void replay_merge(){
    HostConfiguration c;c.max_open_dlus_per_channel=4;Fixture f(c);f.fail_sequence({2,1});
    f.run({wr(1,0,4*4096),rd(2,0,4*4096,{GlobalRequestId{1}})});
    check(f.host.statistics().recovery_tasks==1&&f.host.statistics().replay_passes==2,"nested failure shares task/budget");
    check(f.task().through==2,"earlier re-failure does not shorten endpoint");
}
void persistent_failure(){
    Fixture f;f.device.set_program_outcome([](auto&op){return op.logical.block==0&&op.physical.page==0?ProgramOutcome::FailedReusable:ProgramOutcome::Success;});
    f.run({wr(1,0),wr(2,32768,4096,2),rd(3,0,64,{GlobalRequestId{1}})},false);
    check(f.task().passes==3&&f.task().failure=="RecoveryBudgetExhausted","bounded 3 replay passes");
    check(f.host.results().at(GlobalRequestId{2}).state==LogicalResult::Success,"independent block progresses");
    check(f.host.results().at(GlobalRequestId{3}).reason=="DependencyFailed","dependent read canceled");
}
void missing_retention(){
    HostConfiguration c;c.retained_fragments=0;Fixture f(c);f.fail_sequence({0});
    f.run({wr(1,0),wr(2,32768,4096,2)},false);
    check(f.task().failure=="MissingRecoveryData"&&f.task().passes==0,"missing Host data fails without hidden NAND read");
    check(f.host.results().at(GlobalRequestId{2}).state==LogicalResult::Success,"other block survives retention failure");
}
void replacement_recovery(){
    Fixture f;f.fail_sequence({0},true);f.run({wr(1,0),rd(2,0,4096,{GlobalRequestId{1}})});
    check(f.host.statistics().replay_passes==1,"physical replacement hidden from Host");
}
void zero_budget(){
    HostConfiguration c;c.max_replay_passes=0;Fixture f(c);f.fail_sequence({0});f.run({wr(1,0)},false);
    check(f.task().passes==0&&f.task().failure=="RecoveryBudgetExhausted","zero disables replay");
}
void partial_timeout(){
    HostConfiguration c;c.max_timeout_retries=0; // retained fail-on-timeout baseline
    Fixture f(c);auto r=f.run({wr(1,0,64)},false);
    check(r.at.value>=10000000&&f.host.results().at(GlobalRequestId{1}).reason=="WriteStatus:5","future timeout runs rather than Stalled");
}
void mocs_one(){
    HostConfiguration c;c.writes=WriteCommandMode::Burst4096;Fixture f(c,{},1,1);
    f.run({wr(1,0,8192),rd(2,0,8192,{GlobalRequestId{1}})});
}
void mocs_partial(){
    Fixture f({}, {},1,1,Tick{50000});f.run({wr(1,0)},false);
    check(f.host.statistics().handed==f.host.statistics().responses,"already handed RX drains on logical abort");
}
void cancelled_future_release(){
    Fixture f;auto r=rd(2,0,64,{GlobalRequestId{1}});r.not_before=Tick{2000000000000ULL};
    auto report=f.run({wr(1,0,64),r},false);
    check(report.at.value<20000000,"canceled dependency does not retain obsolete release beyond run limit");
}
void empty(){Fixture f;f.run({});check(f.device.statistics().handoffs==0,"empty no transmissions");}
void not_started(){Fixture f;f.host.load({wr(1,0)});auto r=f.runner.run();check(r.status==RunStatus::Stalled&&r.explanation.find("HostNotStarted")!=std::string::npos,"no start event is Stalled");}
void stalled_dependency(){Fixture f;f.host.load({wr(1,4096)});f.host.start();auto r=f.runner.run();check(r.status==RunStatus::Stalled&&r.host.unfinished_global==1,"missing prior page cannot advance");}
void time_limit(){
    Fixture f;f.host.load({wr(1,0)});f.host.start();RunLimits l;l.max_tick=Tick{100};auto r=f.runner.run(l);
    check(r.status==RunStatus::LimitReached&&!r.host.drained(),"time limit retains attempts");
    r=f.runner.run();check(r.status==RunStatus::Completed&&r.workload_success,"resume preserved state");
}
void event_limit(){
    Fixture f;f.host.load({wr(1,0)});f.host.start();RunLimits l;l.events.total_events=10;auto r=f.runner.run(l);
    check(r.status==RunStatus::LimitReached&&r.events==10,"total callback limit exact");
    r=f.runner.run();check(r.status==RunStatus::Completed&&r.workload_success,"resume event-limited state");
}
void same_tick_limit(){
    Fixture f;f.host.load({});f.host.start();
    std::function<void()> loop;loop=[&]{f.events.schedule_at(f.events.now(),EventPhase::Arrival,loop);};
    f.events.schedule_at(Tick{},EventPhase::Arrival,loop);
    RunLimits l;l.events.events_per_tick=20;auto r=f.runner.run(l);
    check(r.status==RunStatus::LimitReached&&f.events.pending()>0,"same Tick loop bounded without dropping work");
}
void ordered_pending(){
    Fixture f;auto r=rd(2,0);r.group=ReadGroup{1};r.not_before=Tick{10000};
    f.host.load({wr(1,0,64),r});f.host.start();auto report=f.runner.run();
    check(report.status==RunStatus::ModelError&&report.explanation.find("LIMIT-READ-001")!=std::string::npos,"unsupported subset explicit");
}
void read_during_recovery(){
    Fixture f;f.fail_sequence({0});auto r=rd(2,64);r.not_before=Tick{1510000};
    f.run({wr(1,0),r},false);
    check(f.host.results().at(GlobalRequestId{1}).state==LogicalResult::Success,"recovery completes despite ordinary read error");
    check(f.host.results().at(GlobalRequestId{2}).reason=="ReadStatus:2","read during recovery receives restriction");
}
void versions(){
    Fixture f;auto r=rd(3,0,4096,{GlobalRequestId{2}},2);
    f.run({wr(1,0,4096,1),wr(2,0,4096,2),r});
    check(f.host.retained().find({{0,0,0,0},1},0,0)->token!=f.host.retained().find({{0,0,0,0},2},0,0)->token,"immutable retained versions");
}
void data_mismatch(){Fixture f;f.run({wr(1,0),rd(2,0,64,{GlobalRequestId{1}},9)},false);check(f.host.results().at(GlobalRequestId{2}).reason=="DataMismatch","symbolic data verified");}
void delayed_release(){Fixture f;auto w=wr(1,0);w.not_before=Tick{100000000};auto r=f.run({w});check(r.at.value>100000000,"future workload release prevents false Stalled");}
void retention_pins(){
    auto d=profile();ReferenceAddressLayout layout(d.geometry());RoundRobinInterleaver i({},1,d.geometry().channel_bytes());GlobalRequestSplitter s(i,layout);
    WriteRetentionStore store(64);ContentKey a{{0,0,0,0},1},b{{0,0,0,1},2};
    for(auto&f:s.split(wr(1,0)))store.save(a,f);
    check(store.pin_prefix(a,0),"complete prefix pin");
    for(auto&f:s.split(wr(2,32768,4096,2)))store.save(b,f);
    check(store.find(a,0,0).has_value()&&!store.find(b,0,0),"active replay data protected");
    store.unpin(a);for(auto&f:s.split(wr(2,32768,4096,2)))store.save(b,f);
    check(!store.find(a,0,0)&&store.find(b,0,0).has_value(),"unpinned eviction");
}
void recovery_snapshot(){
    RecoveryManager rm(3);ContentKey key{{0,0,0,0},1};
    auto&t=rm.notice(key,0,{}, {CommandId{1},CommandId{2}});
    rm.notice(key,0,PageId{2},{CommandId{1},CommandId{2},CommandId{3}});
    check(t.drain.size()==2&&t.through==2,"duplicate errors don't expand frozen set with new reads");
    rm.settled(key.block,CommandId{1});rm.settled(key.block,CommandId{2});
    check(rm.begin_pass(key,1)&&t.passes==0,"handing pass does not count acceptance");
    check(rm.accepted(key,1,0)&&!rm.accepted(key,1,0)&&t.passes==1,"page0 acceptance exactly once");
    rm.notice(key,1,PageId{1},{CommandId{4}});check(t.through==2&&t.drain.size()==1,"each pass new drain boundary");
}
class BadPolicy:public IHostSendPolicy{public:SendUnitId select(const std::vector<HostSendCandidate>&)const override{return SendUnitId{999999};}};
void policy_guard(){
    Fixture f({}, {},1,256,Tick{10000000},std::make_unique<FifoUciePolicy>(),std::make_unique<BadPolicy>());
    f.host.load({wr(1,0)});f.host.start();auto r=f.runner.run();check(r.status==RunStatus::ImplementationError,"invalid policy cannot bypass legality");
}

void delayed_old_attempts(){
    HostConfiguration hc;hc.max_open_dlus_per_channel=4;
    ExecutionConfiguration ec;ec.media_timing.erase=Tick{1};ec.media_timing.program=Tick{10000};
    ec.ucie.overhead.request_fixed_latency=Tick{100000};ec.buffers.ingress={256,16384};
    Fixture f(hc,ec);f.fail_sequence({0,0});
    std::set<CommandId> live,frozen;std::uint64_t failing_pass=~std::uint64_t{};
    bool saw_link=false;std::size_t barriers=0,root_events=0;
    f.host.set_trace([&](const HostTrace&e){
        std::string action=e.action;
        if(action=="handoff"){
            if(e.pass>0&&e.pass!=failing_pass&&root_events){
                check(frozen.empty(),"new replay page0 waits every frozen old attempt, including late Link fragments");
                ++barriers;
            }
            live.insert(e.command);
        }
        if(action=="program-fail"||action=="follower-fail"){
            if(e.pass!=failing_pass){failing_pass=e.pass;frozen=live;++root_events;
                if(e.pass==0)saw_link=f.device.activity().in_flight_requests>0;}
        }
        if(action=="response"){live.erase(e.command);frozen.erase(e.command);}
    });
    f.run({wr(1,0,8192),rd(2,0,8192,{GlobalRequestId{1}})});
    check(saw_link,"fault really happened with old write fragments still in Link");
    check(root_events==2&&barriers>0&&live.empty(),"repeated failure repeats drain barrier");
    check(f.host.statistics().replay_passes==2&&f.task().through==0,"same recovery two passes");
}
void ingress_old_attempts(){
    HostConfiguration hc;hc.max_open_dlus_per_channel=4;
    ExecutionConfiguration ec;ec.media_timing.erase=Tick{1};ec.media_timing.program=Tick{1000000};ec.buffers.ingress={256,16384};
    // 256 MOCS hold four pages. A fifth page is already in ingress when page0 fails.
    hc.max_open_dlus_per_channel=6;Fixture f(hc,ec);f.fail_sequence({0});
    bool saw_ingress=false;std::set<CommandId> live,frozen;bool notified=false;
    f.host.set_trace([&](const HostTrace&e){
        const std::string action=e.action;
        if(action=="handoff"){if(e.pass)check(frozen.empty(),"replay waits old ingress final responses");live.insert(e.command);}
        if(!notified&&action=="program-fail"){notified=true;saw_ingress=f.device.activity().ingress>0;frozen=live;}
        if(action=="response"){live.erase(e.command);frozen.erase(e.command);}
    });
    f.run({wr(1,0,6*4096)},true);
    check(notified,"failure was observed");
    check(saw_ingress,"old fragments actually resident in ingress at failure");
}
void burst_recovery(){
    HostConfiguration c;c.writes=WriteCommandMode::Burst4096;c.max_open_dlus_per_channel=4;
    Fixture f(c);f.fail_sequence({2,1});f.run({wr(1,0,4*4096),rd(2,0,4*4096,{GlobalRequestId{1}})});
    check(f.host.statistics().recovery_tasks==1&&f.host.statistics().replay_passes==2,"burst replay budget same definition");
}
void multi_channel_recovery(){
    HostConfiguration c;c.max_open_dlus_per_channel=4;Fixture f(c,{},2);
    // Fixture injector applies to block 0 of both channels; faults once total.
    f.fail_sequence({1});f.run({wr(1,0,3*8192),rd(2,0,3*8192,{GlobalRequestId{1}})});
    check(f.host.statistics().recovery_tasks==1,"fault remains local while Global spans channels");
}
void prior_success_retained(){
    Fixture f;f.fail_sequence({1});auto w=wr(2,4096);w.not_before=Tick{2000000};
    f.run({wr(1,0),w,rd(3,0,8192,{GlobalRequestId{1},GlobalRequestId{2}})});
    check(f.host.results().at(GlobalRequestId{1}).state==LogicalResult::Success,"historical Global success preserved through prefix replay");
    check(f.task().through==1&&f.host.statistics().recovered_fragments==64,"only failed work resolved by recovery");
}
void abort_new_content(){
    HostConfiguration hc;hc.max_replay_passes=0;Fixture f(hc);f.fail_sequence({0});
    f.run({wr(1,0,4096,1),wr(2,0,4096,2)},false);
    check(f.host.results().at(GlobalRequestId{2}).reason=="PriorContentFailed","new content cannot overwrite unresolved old recovery");
}
void teardown_host(){
    EventQueue e;DeviceDriver d(profile(),{},e);RoundRobinInterleaver i({},1,d.description().geometry().channel_bytes());
    {
        HostConfiguration c;c.writes=WriteCommandMode::Burst4096;HostDriver h(d,e,i,c);
        h.load({wr(1,0)});h.start();
        while(!h.statistics().handed)check(e.run_next(),"reach handed operation");
    }
    e.run();check(d.activity().drained(),"destroyed Host callbacks do not access freed state");
}
void runner_device_activity(){
    Fixture f;f.host.load({});f.host.start();
    SimulationRunner runner(f.events,f.host,[]{DeviceActivity a;a.bank_operations=1;return a;});
    auto r=runner.run();check(r.status==RunStatus::Stalled,"empty Host alone does not mean completed");
}
void round_limit(){
    Fixture f;f.host.load({});f.host.start();std::function<void()> loop;
    loop=[&]{f.events.schedule_at(f.events.now(),EventPhase::Completion,[&]{f.events.schedule_at(f.events.now(),EventPhase::Arrival,loop);});};
    f.events.schedule_at(Tick{},EventPhase::Arrival,loop);
    RunLimits l;l.events.events_per_tick=0;l.events.rounds_per_tick=5;auto r=f.runner.run(l);
    check(r.status==RunStatus::LimitReached&&f.events.pending()>0,"round limit independently bounds causal cycle");
}



void late_read_second_pass(){
    HostConfiguration hc;hc.max_open_dlus_per_channel=4;
    ExecutionConfiguration ec;ec.media_timing.erase=Tick{1};ec.media_timing.program=Tick{10000};
    ec.ucie.overhead.request_fixed_latency=Tick{100000};ec.buffers.ingress={256,16384};
    Fixture timing(hc,ec);timing.fail_sequence({0,0});timing.run({wr(1,0,8192)});
    Tick second{};
    for(auto&e:timing.trace)if(e.action=="program-fail"&&e.pass==1){second=e.at;break;}
    check(second.value>50000,"locate deterministic second failure");
    auto read=rd(2,7*4096);read.not_before=Tick{second.value-50000};
    Fixture f(hc,ec);f.fail_sequence({0,0});
    bool saw_second_link=false;std::set<CommandId> live,frozen;bool failed_second=false;
    f.host.set_trace([&](const HostTrace&e){
        const std::string a=e.action;
        if(a=="handoff"){if(e.pass==2)check(frozen.empty(),"second restart drains late old read");live.insert(e.command);}
        if(a=="program-fail"&&e.pass==1&&!failed_second){failed_second=true;frozen=live;saw_second_link=f.device.activity().in_flight_requests>0;}
        if(a=="response"){live.erase(e.command);frozen.erase(e.command);}
    });
    f.run({wr(1,0,8192),read},false);
    check(saw_second_link,"second failure has a real old request in Link");
    check(f.host.results().at(GlobalRequestId{1}).state==LogicalResult::Success,"late read doesn't prevent recovery progress");
}


} // namespace
int main(int argc,char**argv){
    std::map<std::string,void(*)()> tests{
        {"delayed_old_attempts",delayed_old_attempts},{"ingress_old_attempts",ingress_old_attempts},{"burst_recovery",burst_recovery},{"multi_channel_recovery",multi_channel_recovery},{"prior_success_retained",prior_success_retained},{"abort_new_content",abort_new_content},{"teardown_host",teardown_host},{"runner_device_activity",runner_device_activity},{"round_limit",round_limit},
        {"late_read_second_pass",late_read_second_pass},{"cancelled_future_release",cancelled_future_release},{"normal",normal},{"sixteen_channels",sixteen_channels},{"burst_compare",burst_compare},{"burst_cross_global",burst_cross_global},
        {"input_validation",input_validation},{"content_validation",content_validation},{"burst_validation",burst_validation},
        {"splitter",splitter},{"parser",parser},{"hints",hints},{"ordered_reads",ordered_reads},{"open_limit",open_limit},
        {"fill_open",fill_open},{"recovery_once",recovery_once},{"follower_before_root",follower_before_root},
        {"replay_merge",replay_merge},{"persistent_failure",persistent_failure},{"missing_retention",missing_retention},
        {"replacement_recovery",replacement_recovery},{"zero_budget",zero_budget},{"partial_timeout",partial_timeout},
        {"mocs_one",mocs_one},{"mocs_partial",mocs_partial},{"empty",empty},{"not_started",not_started},
        {"stalled_dependency",stalled_dependency},{"time_limit",time_limit},{"event_limit",event_limit},
        {"same_tick_limit",same_tick_limit},{"ordered_pending",ordered_pending},{"read_during_recovery",read_during_recovery},
        {"versions",versions},{"data_mismatch",data_mismatch},{"delayed_release",delayed_release},
        {"retention_pins",retention_pins},{"recovery_snapshot",recovery_snapshot},{"policy_guard",policy_guard}
    };
    try{if(argc!=2||!tests.count(argv[1]))throw std::invalid_argument("unknown scenario");tests.at(argv[1])();
        std::cout<<"PASS "<<argv[1]<<" ("<<checks<<" checks)\n";return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL "<<(argc>1?argv[1]:"")<<": "<<e.what()<<"\n";return 1;}
}


