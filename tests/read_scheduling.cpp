#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace {
void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F f) {bool caught=false;try{f();}catch(const std::logic_error&){caught=true;}check(caught,"missing diagnostic");}
StaticDeviceDescription profile() {
    Geometry g{1,1,1,8,4,1};return {g,{256,8,false,false},{256,Tick{10000000},g.block_group_dlus()}};
}
HostConfiguration host_config() {HostConfiguration h;h.hints=ReadHintMode::AllRegular;return h;}
struct Fixture {
    StatisticsCollector stats;EventQueue events;DeviceDriver device;RoundRobinInterleaver inter;
    HostDriver host;SimulationRunner runner;
    Fixture(ExecutionConfiguration config={},bool enabled=true)
        :device(profile(),config,events),inter({},1,device.description().geometry().channel_bytes()),
         host(device,events,inter,host_config()),runner(events,host,device) {
        if(enabled){device.set_statistics_sink(stats);host.set_statistics_sink(stats);}
    }
    void start(std::vector<GlobalRequest> input){host.load(input);host.start();}
    RunReport finish(bool success=true){auto r=runner.run();check(r.status==RunStatus::Completed&&r.workload_success==success,"closed loop result");
        check(r.host.drained()&&r.device.drained(),"closed loop drain");return r;}
};
GlobalRequest write(){return {GlobalRequestId{1},Op::Write,GlobalByteAddress{0},4096,1};}
GlobalRequest read(std::uint64_t id,std::uint64_t offset,std::uint64_t at){
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{offset},64};r.not_before=Tick{at};r.after={GlobalRequestId{1}};return r;
}
void policy_order(){
    ReadPriorityNandPolicy p;FcfsNandPolicy f;
    std::vector<MediaCandidateView> c{{1,1,MediaOperationKind::Erase,{}},{2,2,MediaOperationKind::Program,{}},
                                     {3,8,MediaOperationKind::Read,{}},{4,8,MediaOperationKind::Read,{}}};
    check(p.select(c)==3&&f.select(c)==1,"read priority and deterministic ties");
    c.erase(c.begin()+2,c.end());check(p.select(c)==1,"writes/erases retain FCFS");
    rejects([&]{p.select({});});
}
void window_reasons(){
    WindowDispatchPolicy policy(Tick{200},4);ReadDispatchWindow w(policy);
    std::vector<ReadReleaseReason> reasons;std::vector<Tick> moments;
    w.set_release_observer([&](const auto&,auto reason,Tick first,Tick due,Tick now){
        check(due.value==first.value+200,"first-arrival deadline");reasons.push_back(reason);moments.push_back(now);});
    for(unsigned i=0;i<4;++i)w.arrive(CommandId{i+1},ReadHint::Regular,Tick{40*i});
    check(reasons.size()==1&&reasons[0]==ReadReleaseReason::Threshold&&moments[0]==Tick{120},"count trigger");
    w.arrive(CommandId{5},ReadHint::Regular,Tick{300});w.arrive(CommandId{6},ReadHint::Regular,Tick{350});
    check(!w.flush_due(Tick{499})&&w.flush_due(Tick{500})->size()==2,"timer never reset by later read");
    check(reasons.back()==ReadReleaseReason::Timer,"timer reason");
    w.arrive(CommandId{7},ReadHint::Batch,Tick{600});check(reasons.back()==ReadReleaseReason::Batch,"Batch reason");
    WindowDispatchPolicy zero(Tick{},4);check(zero.release_reason({1,Tick{}},Tick{})==ReadReleaseReason::Immediate,"zero wait");
}
void window_stale(){
    WindowDispatchPolicy p(Tick{200},2);ReadDispatchWindow w(p);unsigned releases=0;
    w.set_release_observer([&](const auto&,auto,auto,auto,auto){++releases;});
    w.arrive(CommandId{1},ReadHint::Regular,Tick{});const auto old=*w.wakeup();
    w.arrive(CommandId{2},ReadHint::Regular,Tick{40});w.arrive(CommandId{3},ReadHint::Regular,Tick{80});
    check(!w.flush_due(old,Tick{200})&&w.size()==1,"old timer cannot release new window");
    check(w.flush_due(*w.wakeup(),Tick{280})->size()==1&&releases==2,"trailing singleton releases autonomously");
}
void cache_bypass(){
    ExecutionConfiguration ec;ec.read_dispatch.wait=Tick{200000};ec.read_dispatch.threshold=0;
    Fixture f(ec);f.start({write(),read(2,0,10000000),read(3,64,11000000)});f.finish();
    auto i=f.stats.read_paths().begin();check(i->second.service==ReadServiceKind::Sense,"first read senses");
    ++i;check(i->second.service==ReadServiceKind::Cache&&!i->second.window_entered_at&&!i->second.bank_ticket,"early hit bypasses window/Bank");
}
void cache_removal(){
    ExecutionConfiguration ec;ec.read_dispatch.wait=Tick{200000};ec.read_dispatch.threshold=2;
    Fixture f(ec);f.start({write(),read(2,0,10000000),read(3,64,10040000),read(4,128,10080000)});f.finish();
    const auto& p=f.stats.read_paths().rbegin()->second;
    check(p.window_entered_at&&p.window_left_at&&!p.release_reason&&p.service==ReadServiceKind::Cache,"cached member removed before release");
    check(f.stats.read_window_releases().size()==1&&f.device.statistics().senses==1,"stale timer creates no extra release/sense");
}
void terminal_read(){
    Fixture f;bool delivered=false;
    HostCommand invalid{CommandId{1},Op::Read,{0,LocalByteAddress{f.device.description().geometry().channel_bytes()}},64};
    const auto result=f.device.try_submit({invalid,{}},[&](const DeliveredResponse& response){
        check(std::get<ReadResponse>(response.response).status==ReadStatus::InvalidAddress,"out-of-range read rejected");delivered=true;});
    check(result.disposition==HandoffDisposition::Taken,"invalid semantic request still enters transport");
    f.events.run();check(delivered&&f.device.activity().drained(),"error response delivered and drained");
    const auto& p=f.stats.read_paths().begin()->second;
    check(!p.window_entered_at&&!p.service&&!p.bank_ticket,"immediate read error has no fictional media/window");
}
void snapshot(){
    ExecutionConfiguration ec;ec.read_dispatch.wait=Tick{200000};ec.read_dispatch.threshold=0;
    Fixture f(ec);f.start({write(),read(2,0,10000000)});
    for(unsigned i=0;;++i){check(i<10000&&f.events.run_next(),"reach active window");
        if(!f.stats.read_paths().empty()&&f.stats.read_paths().begin()->second.window_entered_at)break;}
    const auto& p=f.stats.read_paths().begin()->second;check(!p.window_left_at&&!p.service,"pending observation remains optional");
    std::ostringstream out;f.stats.write_json(out,f.events.now(),"LimitReached");
    check(out.str().find("\"window_left_at\":null")!=std::string::npos,"null snapshot output");
    f.finish();check(f.stats.read_window_releases().size()==1,"resume exactly one release");
}
void observational(){
    ExecutionConfiguration ec;ec.nand_scheduler="read_priority";ec.read_dispatch.wait=Tick{200000};ec.read_dispatch.threshold=2;
    const std::vector<GlobalRequest> input{write(),read(2,0,10000000),read(3,64,10040000),read(4,128,10080000)};
    Fixture on(ec),off(ec,false);on.start(input);off.start(input);auto a=on.finish(),b=off.finish();
    check(a.at==b.at&&a.events==b.events&&on.device.statistics().senses==off.device.statistics().senses,"statistics change no time/events/choices");
}
void guards(){
    StatisticsCollector s;const TransferAttemptId id{1};
    HostCommand command{CommandId{1},Op::Read,{0,LocalByteAddress{0}},64};
    s.observe(StatAttemptStarted{id,command,Tick{0}});
    rejects([&]{s.observe(StatReadWindowEntered{id,Tick{0}});});
    s.observe(StatAttemptAdvanced{id,AttemptMilestone::RxArrival,Tick{1}});
    s.observe(StatAttemptAdvanced{id,AttemptMilestone::Accepted,Tick{1}});
    s.observe(StatReadWindowEntered{id,Tick{1}});
    rejects([&]{s.observe(StatReadWindowEntered{id,Tick{1}});});
    rejects([&]{s.observe(StatReadServed{id,ReadServiceKind::Cache,{},Tick{2}});});
    rejects([&]{s.observe(StatReadWindowReleased{0,{id,id},ReadReleaseReason::Timer,Tick{1},Tick{2},Tick{2}});});
    check(!s.read_paths().at(id).window_left_at,"invalid batch is atomic");
    s.observe(StatReadWindowRemoved{id,Tick{2}});
    rejects([&]{s.observe(StatReadWindowRemoved{id,Tick{2}});});
    s.observe(StatReadServed{id,ReadServiceKind::Cache,{},Tick{3}});
    rejects([&]{s.observe(StatReadServed{id,ReadServiceKind::Cache,{},Tick{3}});});
}
void custom_reason(){
    struct Custom : IReadDispatchPolicy {bool ready(ReadWindowView v,Tick now)const override{return v.size&&now.value>=v.first_arrival.value+7;}
        Tick deadline(ReadWindowView v)const override{return Tick{v.first_arrival.value+7};}} policy;
    ReadDispatchWindow w(policy);ReadReleaseReason seen=ReadReleaseReason::Timer;
    w.set_release_observer([&](const auto&,auto reason,auto,auto,auto){seen=reason;});
    w.arrive(CommandId{1},ReadHint::Regular,Tick{});w.flush_due(Tick{7});
    check(seen==ReadReleaseReason::Policy,"custom policies need no diagnostic override");
}
}
int main(int argc,char** argv){
    const std::map<std::string,void(*)()> cases{{"policy_order",policy_order},{"window_reasons",window_reasons},
        {"window_stale",window_stale},{"cache_bypass",cache_bypass},{"cache_removal",cache_removal},
        {"terminal_read",terminal_read},{"snapshot",snapshot},{"observational",observational},{"guards",guards},{"custom_reason",custom_reason}};
    try{if(argc!=2)throw std::invalid_argument("expected scenario");cases.at(argv[1])();std::cout<<"PASS "<<argv[1]<<'\n';return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
