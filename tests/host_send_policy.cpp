#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include <iostream>
#include <string_view>
using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
StaticDeviceDescription profile(unsigned channels=1){
    Geometry g{channels,1,1,8,4,1};return {g,{256,8,false,true},{256,Tick{10000000},g.block_group_dlus()}};
}
GlobalRequest write(std::uint64_t id,std::uint64_t address,std::uint64_t bytes=4096,Tick at={}){
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{address},bytes,1};r.not_before=at;return r;
}
GlobalRequest read(std::uint64_t id,std::uint64_t address,Tick at={}){
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{address},64};r.not_before=at;return r;
}
struct Observations: IStatisticsSink {
    std::map<TransferAttemptId,StatAttemptLinked> links;
    std::map<GlobalRequestId,Tick> releases;
    void observe(const StatisticsEvent& event) override {
        if(auto p=std::get_if<StatAttemptLinked>(&event))links.emplace(p->id,*p);
        if(auto p=std::get_if<StatGlobalReleased>(&event))releases.emplace(p->id,p->at);
    }
};
// Controlled asynchronous public-port feedback isolates queue order from NAND
// timing. Integration cases below also run the real Device and resource model.
class ScriptPort:public IHostPort {
public:
    struct Record {LocalTransaction tx;Receiver response;TransferAttemptId attempt;Tick handed;bool done=false;};
    EventQueue events;AdmissionWaitRegistry signals{events};
    StaticDeviceDescription desc;ReferenceAddressLayout address_layout;
    std::map<CommandId,Record> records;std::vector<CommandId> order;
    std::map<std::pair<ChannelId,std::uint64_t>,unsigned> attempts;
    std::map<ChannelId,Tick> available;
    std::function<bool(const LocalTransaction&)> blocked;
    std::function<void(CommandId,unsigned)> handler;
    std::uint64_t next=0,spacing=5;
    explicit ScriptPort(unsigned channels=1):desc(profile(channels)),address_layout(desc.geometry()){}
    const StaticDeviceDescription& description()const override{return desc;}
    const IAddressLayout& layout()const override{return address_layout;}
    AdmissionWaitRegistry& notifications()override{return signals;}
    void at(Tick time,std::function<void()> callback){events.schedule_at(time,EventPhase::Arrival,std::move(callback));}
    void gate(ChannelId ch,Tick until){
        available[ch]=until;
        at(until,[this,ch]{signals.notify(WaitKey::channel_key(WaitReason::RequestLink,ch));});
    }
    HandoffResult try_submit(const LocalTransaction& tx,Receiver reply,AcceptanceObserver accepted)override {
        const auto ch=tx.command.address.channel;
        if(events.now()<available[ch])return {HandoffDisposition::NotTaken,{}, {WaitKey::channel_key(WaitReason::RequestLink,ch)}};
        if(blocked&&blocked(tx))return {HandoffDisposition::NotTaken,{}, {WaitKey::channel_key(WaitReason::IngressSpace,ch)}};
        const TransferAttemptId attempt{++next};const auto id=tx.command.id;
        check(records.emplace(id,Record{tx,std::move(reply),attempt,events.now()}).second,"fresh command ID per attempt");
        order.push_back(id);gate(ch,Tick{events.now().value+spacing});
        const auto count=++attempts[{ch,tx.command.address.local.value}];
        at(events.now(),[this,id,attempt,accepted,count,ch]{
            if(accepted)accepted({attempt,ch,id,events.now()});
            if(handler)handler(id,count);else finish(id);
        });
        return {HandoffDisposition::Taken,attempt,{}};
    }
    void finish(CommandId id,WriteStatus status=WriteStatus::Success,std::uint8_t info=0,std::uint64_t delay=1){
        at(Tick{events.now().value+delay},[this,id,status,info]{
            auto& r=records.at(id);check(!r.done,"one final response per attempt");r.done=true;
            const auto ch=r.tx.command.address.channel;
            if(r.tx.command.op==Op::Write)r.response({r.attempt,WriteResponse{ch,id,status,info},events.now()});
            else r.response({r.attempt,ReadResponse{ch,id,ReadStatus::Success,DataToken{1}},events.now()});
        });
    }
    std::uint64_t address(CommandId id)const{return records.at(id).tx.command.address.local.value;}
    std::vector<std::uint64_t> addresses()const{
        std::vector<std::uint64_t> result;for(auto id:order)result.push_back(address(id));return result;
    }
};
struct ScriptFixture {
    ScriptPort port;Observations observations;RoundRobinInterleaver inter;HostDriver host;
    explicit ScriptFixture(HostConfiguration c={},unsigned channels=1)
        :port(channels),inter(GlobalByteAddress{},channels,port.desc.geometry().channel_bytes()),host(port,port.events,inter,c){
        host.set_statistics_sink(observations);
    }
    void run(std::vector<GlobalRequest> input){
        host.load(input);host.start();port.events.set_limits({200000,10000,10000});port.events.run();
        check(host.activity().drained()&&host.workload_success(),"successful drained Host");
        check(host.statistics().handed==host.statistics().responses,"all handed attempts settled");
        for(const auto& r:port.records)check(r.second.done,"no lost old attempt");
    }
};
HostConfiguration burst(){HostConfiguration c;c.writes=WriteCommandMode::Burst4096;return c;}
void policy_order(){
    const std::vector<HostSendCandidate> candidates{{SendUnitId{1},true,true,9},{SendUnitId{8},false,false,2},{SendUnitId{4},false,true,4}};
    FcfsHostSendPolicy fcfs;FillOpenDluPolicy fill;
    check(fcfs.select(candidates)==SendUnitId{8},"FCFS uses enqueue age, not ID or replay priority");
    check(fill.select(candidates)==SendUnitId{1},"fill_open retains supplement priority");
    for(auto name:{"fcfs","fill_open"}){
        auto p=make_host_send_policy(name);bool threw=false;try{p->select({});}catch(const std::logic_error&){threw=true;}
        check(threw,"empty candidate diagnostic");
    }
    check(fcfs.select({{SendUnitId{3},false,false,1},{SendUnitId{2},true,true,1}})==SendUnitId{2},"stable tie breaker");
}
void real_policy(const std::string& policy,unsigned open,std::size_t expected){
    EventQueue events;DeviceDriver device(profile(),{},events);
    RoundRobinInterleaver inter(GlobalByteAddress{},1,device.description().geometry().channel_bytes());
    HostConfiguration c;c.send_policy=policy;c.max_open_dlus_per_channel=open;
    HostDriver host(device,events,inter,c);SimulationRunner runner(events,host,device);
    std::size_t before=0;bool seen_other=false;
    host.set_trace([&](const HostTrace& e){if(std::string_view(e.action)=="handoff"){
        if(e.block.block==1)seen_other=true;else if(!seen_other)++before;
    }});
    host.load({write(1,0,2048),write(2,32768),write(3,2048,2048)});host.start();const auto r=runner.run();
    check(r.status==RunStatus::Completed&&r.workload_success&&r.host.drained()&&r.device.drained(),"real closed loop drains");
    check(device.statistics().programs==2&&host.statistics().handed==128,"two pages programmed without retries");
    check(before==expected,"observed policy-specific handoff order");
}
void fcfs(){real_policy("fcfs",2,32);}
void fill_open(){real_policy("fill_open",2,64);}
void blocked_open(){real_policy("fcfs",1,64);}
void release_order(){
    ScriptFixture f;f.port.gate(0,Tick{20});f.run({read(1,0,Tick{10}),read(2,64)});
    check(f.port.addresses()==std::vector<std::uint64_t>{64,0},"release time precedes preload creation order");
}
void same_tick(){
    std::vector<std::uint64_t> first;
    for(unsigned i=0;i<2;++i){
        ScriptFixture f;f.run({read(9,128),read(1,64),read(7,0)});
        check(f.port.addresses()==std::vector<std::uint64_t>{128,64,0},"same-tick input order, not Global ID");
        if(!i)first=f.port.addresses();else check(first==f.port.addresses(),"deterministic replay of input");
    }
}
void dependency_age(){
    ScriptFixture f;auto dependent=read(1,0);dependent.after={GlobalRequestId{90}};
    f.run({dependent,read(90,64),read(2,128,Tick{1})});
    check(f.port.addresses()==std::vector<std::uint64_t>{64,0,128},"dependency bypass retains old request age");
}
void not_taken(){
    ScriptFixture f;f.port.blocked=[&](const LocalTransaction& tx){return tx.command.address.local.value==0&&f.port.events.now()<Tick{3};};
    f.port.at(Tick{3},[&]{f.port.signals.notify(WaitKey::channel_key(WaitReason::IngressSpace,0));});
    f.run({read(1,0),read(2,64),read(3,128)});
    check(f.port.addresses()==std::vector<std::uint64_t>{64,0,128},"NotTaken does not move request to tail");
    const auto a=f.observations.links.at(f.port.records.at(f.port.order[1]).attempt);
    check(a.queued_at==Tick{}&&a.unit_attempt==1,"backpressure is neither new enqueue nor failed attempt");
}
void pending_retry(bool concurrent_release){
    auto c=burst();c.pending_retry_delay=Tick{1};ScriptFixture f(c);f.port.spacing=10;
    f.port.handler=[&](CommandId id,unsigned attempt){
        f.port.finish(id,f.port.address(id)==0&&attempt==1?WriteStatus::PendingDluLimit:WriteStatus::Success,0,0);
    };
    f.run({write(1,0),write(2,32768,4096,Tick{concurrent_release?1ULL:0ULL})});
    check(f.port.addresses()==std::vector<std::uint64_t>{0,32768,0},"eligible retry enters tail behind queued work");
    const auto first=f.observations.links.at(f.port.records.at(f.port.order[0]).attempt);
    const auto retry=f.observations.links.at(f.port.records.at(f.port.order[2]).attempt);
    check(first.unit==retry.unit&&first.work==retry.work&&retry.unit_attempt==2,"retry retains logical work and SendUnit identity");
    check(first.queued_at==Tick{}&&retry.queued_at==Tick{1}&&f.observations.releases.at(GlobalRequestId{1})==Tick{},"only attempt queue time changes");
    if(concurrent_release)check(f.observations.links.at(f.port.records.at(f.port.order[1]).attempt).queued_at==retry.queued_at,"same Tick ordered by enqueue sequence");
}
void retry_tail(){pending_retry(false);}
void same_tick_retry(){pending_retry(true);}
void timeout_retry(bool unsent){
    ScriptFixture f;f.port.spacing=unsent?5:1;
    std::map<std::uint64_t,std::vector<CommandId>> held;
    std::map<std::uint64_t,std::bitset<64>> received;
    bool expired=false;
    f.port.handler=[&](CommandId id,unsigned){
        const auto a=f.port.address(id);const auto page=a/4096;
        check(!received[page].test(a%4096/64),"no duplicate fragment stored in page attempt");
        received[page].set(a%4096/64);held[page].push_back(id);
        if((page!=0||expired)&&received[page].all()){
            for(auto command:held[page])f.port.finish(command);held[page].clear();
        }
    };
    f.port.at(Tick{unsent?11ULL:100ULL},[&]{
        expired=true;for(auto id:held[0])f.port.finish(id,WriteStatus::AssemblyTimeout,0,0);
        held[0].clear();received[0].reset();
    });
    std::vector<GlobalRequest> input{write(1,0)};
    if(!unsent)input.push_back(write(2,32768,4096,Tick{100}));
    f.run(input);const auto addresses=f.port.addresses();
    check(f.host.statistics().timeout_retry_rounds==1,"one retry round for all timeout responses");
    if(unsent){
        check(addresses[3]==192&&addresses[addresses.size()-3]==0,"unsent fragments keep age ahead of retransmissions");
        check(f.host.statistics().timeout_retry_commands==3,"unsent fragments not counted as retries");
    }else{
        check(addresses[64]==32768&&addresses[127]==32768+4032&&addresses[128]==0,"released page precedes requeued timeout page");
        check(f.host.statistics().timeout_retry_commands==64,"all timed-out fragments retried");
    }
}
void timeout_tail(){timeout_retry(false);}
void timeout_unsent(){timeout_retry(true);}
void replay_followers(){
    ScriptFixture f(burst());
    f.port.handler=[&](CommandId id,unsigned attempt){
        const auto a=f.port.address(id);
        if(a==0&&attempt==1)f.port.finish(id,WriteStatus::ProgramFail,0,10);
        else if(a==4096&&attempt==1)f.port.finish(id,WriteStatus::WriteOrderViolation,2);
        else if(a==0&&attempt==2){f.port.gate(0,Tick{30});f.port.finish(id,WriteStatus::Success,0,10);}
        else f.port.finish(id);
    };
    f.run({write(1,0,8192),write(2,32768,4096,Tick{20})});
    check(f.port.addresses()==std::vector<std::uint64_t>{0,4096,0,32768,4096},"failed follower restored after replay reenters tail");
    check(f.host.statistics().recovery_tasks==1&&f.host.statistics().replay_passes==1,"replay merge unchanged");
}
void burst_mixed(){
    ScriptFixture f(burst());f.run({write(1,0),read(2,32768)});
    check(f.port.addresses()==std::vector<std::uint64_t>{0,32768},"burst order independent of deferred burst object construction");
}
void cross_global_burst(){
    for(bool delayed:{false,true}){
        ScriptFixture f(burst());f.port.gate(0,Tick{20});
        f.run({write(1,0,2048),read(2,32768,Tick{delayed?5ULL:0ULL}),write(3,2048,2048,Tick{delayed?10ULL:0ULL})});
        check(f.port.addresses()==std::vector<std::uint64_t>{32768,0},"burst enqueues when last contributor becomes available");
        const auto link=f.observations.links.at(f.port.records.at(f.port.order[1]).attempt);
        check(link.work.size()==64&&link.queued_at==Tick{delayed?10ULL:0ULL},"cross-Global burst preserves work and release boundary");
    }
}
void channels(){
    ScriptFixture f({},2);f.port.gate(0,Tick{100});
    auto r=read(1,0);r.bytes=256;f.run({r});
    const auto& v=f.port.order;
    check(f.port.records.at(v[0]).tx.command.address.channel==1&&f.port.records.at(v[1]).tx.command.address.channel==1,"busy channel does not impose global HOL blocking");
    check(f.port.addresses()==std::vector<std::uint64_t>{0,64,0,64},"per-channel FCFS order");
}
} // namespace
int main(int argc,char** argv){
    const std::map<std::string,void(*)()> cases{{"order",policy_order},{"fcfs",fcfs},{"fill_open",fill_open},
        {"blocked_open",blocked_open},{"release_order",release_order},{"same_tick",same_tick},{"dependency_age",dependency_age},
        {"not_taken",not_taken},{"retry_tail",retry_tail},{"same_tick_retry",same_tick_retry},{"timeout_tail",timeout_tail},
        {"timeout_unsent",timeout_unsent},{"replay_followers",replay_followers},{"burst_mixed",burst_mixed},
        {"cross_global_burst",cross_global_burst},{"channels",channels}};
    try{if(argc!=2||!cases.count(argv[1]))throw std::invalid_argument("unknown Host send policy test");
        cases.at(argv[1])();std::cout<<"PASS "<<argv[1]<<" checks="<<checks<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
