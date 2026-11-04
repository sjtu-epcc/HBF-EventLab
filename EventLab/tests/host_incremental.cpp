#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include "hbf_eventlab/adapters/workload_reader.hpp"
#include <iostream>
#include <sstream>

using namespace hbf_eventlab;
namespace {
void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F fn,const std::string& message) {
    try {fn();}catch(const std::exception& e) {
        check(std::string(e.what()).find(message)!=std::string::npos,e.what());return;
    }
    throw std::runtime_error("missing rejection: "+message);
}
StaticDeviceDescription profile(unsigned channels) {
    Geometry g{channels,1,1,8,4,1};return {g,{256,8,false,true},{256,Tick{10000000},g.block_group_dlus()}};
}
GlobalRequest write(std::uint64_t id,std::uint64_t address,std::uint64_t bytes=4096,std::uint64_t content=1,Tick at={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{address},bytes,content};r.not_before=at;return r;
}
GlobalRequest read(std::uint64_t id,std::uint64_t address,std::uint64_t bytes=64,std::uint64_t content=1,Tick at={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{address},bytes};r.expected_content=content;r.not_before=at;return r;
}
struct Fixture {
    StatisticsCollector stats; // Observer outlives both Host and Device.
    EventQueue events;
    DeviceDriver device;
    RoundRobinInterleaver inter;
    HostDriver host;
    std::vector<GlobalCompletion> completions;
    Fixture(HostConfiguration hc={},unsigned channels=1)
      :device(profile(channels),{},events),inter({},channels,device.description().geometry().channel_bytes()),host(device,events,inter,hc) {
        events.set_limits({1000000,100000,100000});
        device.set_statistics_sink(stats);host.set_statistics_sink(stats);
    }
    void observe() {host.set_completion_handler([this](const auto& c){completions.push_back(c);});}
    void start() {host.start_incremental();}
    void drain() {events.run();check(host.activity().drained() && device.activity().drained(),"temporary state drained");}
    void before(Tick t) {while(auto next=events.next_time()) {if(!(*next<t))break;events.run_next();}}
    bool success(std::uint64_t id) const {return host.results().at(GlobalRequestId{id}).state==LogicalResult::Success;}
    void fail_once(PageId page=0) {
        device.set_program_outcome([page,failed=false](const auto& op)mutable {
            if(!failed && op.physical.page==page){failed=true;return ProgramOutcome::FailedReusable;}
            return ProgramOutcome::Success;
        });
    }
    std::string json() {std::ostringstream out;stats.write_json(out,events.now(),"Drained");return out.str();}
};
void split() {
    Fixture f;f.observe();f.start();f.host.submit(write(1,0,2048));
    f.events.schedule_at(Tick{100000},EventPhase::Arrival,[&]{
        check(f.device.activity().pending_dlus==1,"first half remains active");
        f.host.submit(write(2,2048,2048,1,f.events.now()));
    });
    f.drain();check(f.success(1)&&f.success(2)&&f.device.statistics().programs==1,"cross-submission DLU assembly");
    check(f.completions.size()==2,"two Global completions");
}
void feedback() {
    Fixture f;
    f.host.set_completion_handler([&](const GlobalCompletion& c){
        f.completions.push_back(c);
        check(c.result.state==LogicalResult::Success && c.at==f.events.now(),"stable logical result time");
        if(c.result.id.value==1)f.host.submit(write(2,4096,4096,1,f.events.now()));
        if(c.result.id.value==2)f.host.submit(read(3,0,8192,1,f.events.now()));
    });
    f.start();f.host.submit(write(1,0));check(f.completions.empty(),"no inline completion");f.drain();
    check(f.completions.size()==3&&f.success(3)&&f.device.statistics().programs==2,"feedback extends same version");
}
void idle() {
    Fixture f;f.start();f.drain();f.host.submit(write(1,0));f.drain();
    f.host.submit(read(2,0,64,1,f.events.now()));f.drain();
    check(f.success(2)&&f.device.statistics().erases==1,"idle preserves medium");
    SimulationRunner runner(f.events,f.host,f.device);
    check(runner.run().status==RunStatus::ModelError,"finite runner cannot declare online input completed");
}
void atomic() {
    Fixture f;f.start();f.host.submit(write(1,0));f.before(Tick{10000});
    const auto snapshot=f.json();const auto pending=f.events.pending();
    auto invalid=write(3,32768,63,1,f.events.now());
    rejects([&]{f.host.submit(std::vector<GlobalRequest>{write(2,0,4096,2,f.events.now()),invalid});},"unaligned");
    check(f.json()==snapshot&&f.events.pending()==pending&&f.host.results().size()==1,"rejected batch has no side effects");
    f.host.submit(write(2,4096,4096,1,f.events.now()));f.drain();
    check(f.success(2),"failed new-version admission did not seal old version or consume ID");
}
void duplicate() {
    Fixture f;f.start();f.host.submit(write(1,0));
    rejects([&]{f.host.submit(write(1,4096));},"duplicate Global");
    rejects([&]{f.host.submit(std::vector<GlobalRequest>{write(2,4096),write(3,0)});},"LIMIT-CONTENT-002");
    f.host.submit(write(2,4096));f.drain();check(f.success(2),"duplicate rejection rolls back whole batch");
}
void address_atomic() {
    Fixture f;f.start();f.host.submit(write(1,0));
    const auto snapshot=f.json();
    const auto outside=f.device.description().geometry().channel_bytes();
    rejects([&]{f.host.submit(std::vector<GlobalRequest>{write(2,0,4096,2),read(3,outside)});},"outside");
    check(f.json()==snapshot,"invalid late range leaves all registration unchanged");
    f.host.submit(write(2,4096));f.drain();check(f.success(2),"range rejection does not close old content");
}
void sealed() {
    Fixture f;f.observe();f.start();f.host.submit(write(1,0,4096,9));
    f.host.submit(write(2,0,4096,2)); // Labels need not be numerically increasing.
    const auto handed=f.host.statistics().handed;
    rejects([&]{f.host.submit(write(3,4096,4096,9));},"sealed");
    check(f.host.statistics().handed==handed,"sealed input is not a Device error command");
    f.drain();check(f.success(1)&&f.success(2)&&f.completions.size()==2,"old version drains before rewrite");
    check(f.completions[0].result.id==GlobalRequestId{1},"old version completes first");
    f.host.submit(read(3,0,64,2,f.events.now()));f.drain();check(f.success(3),"new content visible");
}
void block_scope() {
    Fixture f;f.start();f.host.submit(std::vector<GlobalRequest>{write(1,0),write(2,32768,4096,9),write(3,32768,4096,2)});
    f.host.submit(write(4,4096));f.drain();check(f.success(4),"other block versions do not seal this block");
}
void dependencies() {
    Fixture f;f.start();auto r=read(2,0);r.after={GlobalRequestId{1}};
    f.host.submit(std::vector<GlobalRequest>{r,write(1,0)});f.drain();check(f.success(2),"same batch forward dependency");
    r=read(3,0,64,1,f.events.now());r.after={GlobalRequestId{1}};
    f.host.submit(r);f.drain();check(f.success(3),"dependency on previous completed submission");
}
void invalid_dependencies() {
    Fixture f;f.start();auto a=read(1,0),b=read(2,0);a.after={GlobalRequestId{2}};
    rejects([&]{f.host.submit(a);},"unknown read dependency");
    b.after={GlobalRequestId{1}};
    rejects([&]{f.host.submit(std::vector<GlobalRequest>{a,b});},"cyclic");
    auto w=write(3,0);w.after={GlobalRequestId{1}};
    rejects([&]{f.host.submit(w);},"LIMIT-WRITE-001");
    check(f.host.results().empty()&&f.stats.globals().empty(),"dependency rejection is atomic");
}
void burst() {
    HostConfiguration hc;hc.writes=WriteCommandMode::Burst4096;Fixture f(hc);f.start();
    rejects([&]{f.host.submit(write(1,0,2048));},"BURST-INCOMPLETE");
    f.host.submit(std::vector<GlobalRequest>{write(1,0,2048),write(2,2048,2048)});f.drain();
    check(f.success(1)&&f.success(2)&&f.host.statistics().handed==1,"explicit same-batch cross-Global burst");
    f.host.submit(write(3,4096,4096,1,f.events.now()));f.drain();check(f.success(3),"incremental complete-page burst");
}
void future() {
    Fixture f;f.start();f.host.submit(write(1,0,2048,1,Tick{100000}));
    f.host.submit(write(2,2048,2048,1,Tick{100000}));
    f.before(Tick{100000});check(!f.host.statistics().handed,"no early release");
    f.drain();check(f.success(1)&&f.success(2),"merged future release event");
    for(auto id:{1u,2u})check(f.stats.globals().at(GlobalRequestId{id}).released==Tick{100000},"one release per Global");
    rejects([&]{f.host.submit(read(3,0));},"in the past");
}
void future_cancel_reuse() {
    HostConfiguration hc;hc.max_timeout_retries=0;Fixture f(hc);f.start();
    auto r=read(2,0,64,1,Tick{100000000});r.after={GlobalRequestId{1}};
    f.host.submit(std::vector<GlobalRequest>{write(1,0,64),r});f.drain();
    check(f.events.now().value<100000000&&!f.success(2),"canceled future release retired");
    f.host.submit(read(3,32768,64,1,Tick{100000000}));f.drain();
    check(f.stats.globals().at(GlobalRequestId{3}).released==Tick{100000000},"new release at reused tick works");
}
void recovered_sealed() {
    Fixture f;f.observe();f.fail_once();f.start();
    f.host.submit(write(1,0));f.host.submit(write(2,0,4096,2));f.drain();
    check(f.success(1)&&f.success(2)&&f.host.statistics().replay_passes==1,"sealed old version still replays");
    check(f.completions.size()==2&&f.completions[0].result.state==LogicalResult::Success,"failed attempts do not duplicate logical completion");
}
void abort_append() {
    HostConfiguration hc;hc.max_timeout_retries=0;Fixture f(hc);f.observe();f.start();
    f.host.submit(write(1,0,64));f.drain();
    check(f.completions.size()==1&&f.completions[0].result.state==LogicalResult::Failed,"timeout final notification");
    rejects([&]{f.host.submit(write(2,64,64,1,f.events.now()));},"already aborted");
    check(f.host.results().size()==1,"late missing fragments do not revive failed version");
}
void recovery_append() {
    Fixture f;f.observe();f.fail_once();f.start();f.host.submit(write(1,0));
    f.events.schedule_at(Tick{1600000},EventPhase::Arrival,[&]{
        check(f.host.recovery().active_count()!=0,"append while replay is active");
        f.host.submit(write(2,4096,4096,1,f.events.now()));
    });
    f.drain();check(f.success(1)&&f.success(2)&&f.completions.size()==2,"new work waits for existing recovery");
}
void hints() {
    Fixture f({},2);f.start();f.host.submit(write(1,0,8192));f.drain();
    f.host.submit(read(2,0,8192,1,f.events.now()));f.host.submit(read(3,0,64,1,f.events.now()));f.drain();
    std::size_t batches=0;
    for(const auto& a:f.stats.attempts())if(a.second.command.op==Op::Read&&a.second.command.read_hint==ReadHint::Batch)++batches;
    check(batches==3&&f.success(2)&&f.success(3),"TailBatch per Global/channel, not submission call");
}
void limit_atomic() {
    HostConfiguration hc;hc.max_logical_fragments=128;Fixture f(hc);f.start();f.host.submit(write(1,0));
    rejects([&]{f.host.submit(std::vector<GlobalRequest>{write(2,0,4096,2),write(3,32768)});},"fragment limit");
    f.host.submit(write(2,4096));f.drain();check(f.success(2),"lifetime budget failure did not seal version");
    rejects([&]{f.host.submit(read(3,0,64,1,f.events.now()));},"fragment limit");
}
void equivalent() {
    auto r=read(3,0,8192);r.after={GlobalRequestId{1},GlobalRequestId{2}};
    std::vector<GlobalRequest> input{write(1,0),write(2,4096,4096,1,Tick{20000}),r};
    Fixture offline,online;offline.host.load(input);offline.host.start();online.start();
    online.host.submit(input[0]);online.host.submit(std::vector<GlobalRequest>{input[1],input[2]});
    offline.drain();online.drain();check(offline.json()==online.json(),"offline/incremental timings, attempts, resource histories identical");
}
void observer() {
    Fixture plain,observed;observed.observe();
    plain.start();observed.start();plain.host.submit(write(1,0));observed.host.submit(write(1,0));
    plain.drain();observed.drain();
    check(plain.json()==observed.json()&&observed.completions.size()==1,"notification is observational when no new work is submitted");
}
void parser() {
    const std::string text="HBF_EVENTLAB_WORKLOAD 1\n# ignored\nWRITE 1 0 4096 content=1\nREAD 2 0 64 after=1 at=20\n";
    std::istringstream stream(text),whole(text);WorkloadReader reader(stream);
    auto first=reader.next();check(first&&first->id==GlobalRequestId{1},"first record");
    auto second=reader.next();check(second&&second->after==std::vector<GlobalRequestId>{GlobalRequestId{1}},"second record");
    check(!reader.next()&&!reader.next()&&read_workload(whole).size()==2,"EOF and offline wrapper");
    std::istringstream bad("HBF_EVENTLAB_WORKLOAD 1\nREAD 1 0 64\nBROKEN\n");WorkloadReader partial(bad);
    check(partial.next().has_value(),"incremental parser returns valid prefix");
    rejects([&]{partial.next();},"line 3");
}
void guards() {
    Fixture f;rejects([&]{f.host.submit(write(1,0));},"start_incremental");
    f.start();rejects([&]{f.host.start_incremental();},"already selected");
    rejects([&]{f.host.load({});},"already loaded");
    rejects([&]{f.observe();},"before load/start");
    f.host.submit(std::vector<GlobalRequest>{});f.drain();
    Fixture offline;offline.host.load({});offline.host.start();
    rejects([&]{offline.host.submit(read(1,0));},"start_incremental");
}
}
int main(int argc,char** argv) {
    const std::map<std::string,void(*)()> cases={{"split",split},{"feedback",feedback},{"idle",idle},
        {"atomic",atomic},{"duplicate",duplicate},{"address_atomic",address_atomic},{"sealed",sealed},{"block_scope",block_scope},
        {"dependencies",dependencies},{"invalid_dependencies",invalid_dependencies},{"burst",burst},
        {"future",future},{"future_cancel_reuse",future_cancel_reuse},{"recovered_sealed",recovered_sealed},
        {"abort_append",abort_append},{"recovery_append",recovery_append},{"hints",hints},{"limit_atomic",limit_atomic},{"equivalent",equivalent},{"observer",observer},
        {"parser",parser},{"guards",guards}};
    try {if(argc!=2)throw std::runtime_error("one scenario required");cases.at(argv[1])();std::cout<<"PASS incremental Host "<<argv[1]<<'\n';}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
