#include "hbf_eventlab/configuration/simulation_configuration.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/statistics/event_stream.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace hbf_eventlab;
namespace {
void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F f) {
    try {f();}catch(const std::invalid_argument&) {return;}
    throw std::runtime_error("expected input rejection");
}
std::string snapshot;
SimulationConfiguration configuration() {
    SimulationConfiguration c;c.device.geometry={1,1,1,8,4,1};
    c.host.max_resident_fragments=256;return c;
}
GlobalRequest write(std::uint64_t id,std::uint64_t address=0,std::uint64_t bytes=4096,Tick at={},std::uint64_t content=1) {
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{address},bytes,content};r.not_before=at;return r;
}
GlobalRequest read(std::uint64_t id,std::uint64_t address=0,std::uint64_t bytes=64,Tick at={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{address},bytes};r.not_before=at;r.expected_content=1;return r;
}
struct AuditSink final:IStatisticsSink {
    std::ostringstream text;
    StatisticsEventStream stream{text,4096};
    std::function<void()> audit;
    void observe(const StatisticsEvent& e) override {
        stream.observe(e);
        // Check state even when an observer is called from registration/completion.
        if(audit)audit();
    }
};
struct Fixture {
    ResolvedSimulationConfiguration config;
    AuditSink sink;
    EventQueue events;
    DeviceDriver device;
    RoundRobinInterleaver interleaver;
    HostDriver host;
    std::map<GlobalRequestId,unsigned> notifications;
    std::function<void(const GlobalCompletion&)> feedback;
    bool partial=false,error_while_pending=false;
    explicit Fixture(SimulationConfiguration c=configuration())
        :config(resolve_configuration(c)),device(config.description(),config.values().execution,events),
         interleaver(c.address.global_base,c.device.geometry.host_channels,c.device.geometry.channel_bytes(),c.address.interleave_bytes),
         host(device,events,interleaver,c.host) {
        events.set_limits(c.run.events);
        device.set_program_outcome(make_program_outcome(c.faults));
        sink.audit=[this]{oracle();};device.set_statistics_sink(sink);host.set_statistics_sink(sink);
        host.set_completion_handler([this](const GlobalCompletion& e) {
            oracle();check(++notifications[e.result.id]==1,"Global completion delivered exactly once");
            check(e.result.state==host.results().at(e.result.id).state,"callback carries final result");
            if(feedback)feedback(e);
            oracle();
        });
    }
    void oracle() {
        std::size_t pending=0,failed=0;
        for(const auto& entry:host.results()) {
            const auto& r=entry.second;
            pending+=r.state==LogicalResult::Pending;failed+=r.state==LogicalResult::Failed;
            check((r.state==LogicalResult::Pending)==(r.remaining!=0),"remaining/final-state invariant");
            partial|=r.state==LogicalResult::Pending&&r.remaining<64;
        }
        check(host.activity().unfinished_global==pending,"unfinished count equals independent full scan");
        check(host.workload_success()==(pending==0&&failed==0),"success query equals independent full scan");
        error_while_pending|=host.statistics().attempt_errors!=0&&pending!=0;
    }
    void start() {host.start_incremental();oracle();}
    void submit(const GlobalRequest& r) {host.submit(r);oracle();}
    void submit(const std::vector<GlobalRequest>& r) {host.submit(r);oracle();}
    void step() {events.run_next();oracle();}
    void drain() {
        oracle();while(events.next_time())step();
        check(host.activity().drained()&&device.activity().drained(),"temporary activity drains");
        check(notifications.size()==host.results().size(),"every registered Global notified");
    }
    void save() {
        oracle();sink.stream.finish(events.now(),host.workload_success()?"Success":"Failed");
        snapshot+=sink.text.str();
        std::ostringstream out;
        out<<"SUMMARY "<<events.now().value<<' '<<events.executed()<<' '<<host.workload_success()<<' '
           <<host.activity().drained()<<' '<<device.activity().drained()<<'\n';
        for(const auto& entry:host.results())out<<entry.first.value<<' '<<unsigned(entry.second.state)<<' '
            <<entry.second.remaining<<' '<<entry.second.failed<<' '<<entry.second.reason<<' '<<notifications[entry.first]<<'\n';
        const auto& h=host.statistics();
        out<<"HOST "<<h.handed<<' '<<h.accepted<<' '<<h.responses<<' '<<h.attempt_errors<<' '
           <<h.logical_success<<' '<<h.logical_failed<<' '<<h.replay_passes<<' '
           <<h.pending_retry_commands<<' '<<h.timeout_retry_rounds<<'\n';
        snapshot+=out.str();
    }
};
void empty() {
    Fixture online;online.start();online.drain();check(online.host.workload_success(),"empty online result set succeeds");online.save();
    Fixture finite;finite.host.load({});finite.oracle();finite.host.start();finite.drain();finite.save();
}
void partial() {
    Fixture f;f.start();f.submit(write(40));
    check(f.host.activity().unfinished_global==1,"one Global, not 64 fragments");f.drain();
    check(f.partial&&f.host.workload_success(),"partial fragment completions keep Global pending");f.save();
}
void admission() {
    auto c=configuration();c.host.max_resident_fragments=64;Fixture f(c);f.start();
    check(f.host.try_submit(write(1,0,8192)).disposition==InputDisposition::TooLarge,"TooLarge rejected");f.oracle();
    auto bad=write(2,0,63);
    rejects([&]{f.host.submit(std::vector<GlobalRequest>{write(1),bad});});f.oracle();
    check(f.host.results().empty(),"invalid batch never counted");f.submit(write(1));
    check(f.host.try_submit(read(2)).disposition==InputDisposition::Backpressured,"capacity rejection");f.oracle();
    rejects([&]{f.host.submit(read(1));});f.oracle();
    check(f.host.results().size()==1,"rejections preserve result set");f.drain();
    check(f.host.storage().work==0,"budget returned after safe reclamation");
    f.submit(read(2,0,64,f.events.now()));f.drain();f.save();
}
void future() {
    Fixture f;f.start();f.submit(write(99,0,4096,Tick{100000000}));
    check(f.host.activity().unfinished_global==1&&!f.host.statistics().handed,"future input counted at registration");
    while(f.events.next_time()&&*f.events.next_time()<Tick{100000000})f.step();
    check(!f.host.workload_success()&&f.host.activity().unfinished_global==1,"waiting for release stays pending");f.drain();f.save();
}
void feedback() {
    Fixture f;f.feedback=[&](const GlobalCompletion& e) {
        check(f.host.activity().unfinished_global==0,"logical result settled before notification");
        if(e.result.id.value==30)f.submit(write(10,4096,4096,f.events.now()));
        if(e.result.id.value==10)f.submit(read(20,0,8192,f.events.now()));
        if(e.result.id.value!=20)check(f.host.activity().unfinished_global==1,"feedback reopens pending count");
    };
    f.start();f.submit(write(30));
    while(f.host.activity().unfinished_global)f.step();
    check(f.notifications.empty()&&!f.host.activity().drained(),"zero pending does not discard queued completion callback");
    f.drain();check(f.notifications.size()==3&&f.host.workload_success(),"feedback chain succeeds");f.save();
}
void replay() {
    auto c=configuration();c.faults.fail_sequence={0};Fixture f(c);f.start();
    f.submit(std::vector<GlobalRequest>{write(30),write(10,0,4096,{},2)});f.drain();
    check(f.error_while_pending&&f.host.statistics().replay_passes==1&&f.host.workload_success(),"sealed content replays without new Globals");f.save();
}
void pending_retry() {
    auto c=configuration();c.device.capabilities.max_pending_write_dlus=1;c.host.max_open_dlus_per_channel=3;
    c.host.pending_retry_delay=Tick{10000000};c.execution.media_timing.program=Tick{3000000};
    Fixture f(c);f.start();f.submit(std::vector<GlobalRequest>{write(9),write(2,32768)});f.drain();
    check(f.error_while_pending&&f.host.statistics().pending_retry_commands>0&&f.host.workload_success(),"Pending-limit attempts do not finish Globals early");f.save();
}
void timeout_retry() {
    auto c=configuration();c.device.selected.mocs_selected=1;c.device.selected.assembly_timeout=Tick{50000};c.host.max_timeout_retries=1;
    Fixture f(c);f.start();f.submit(write(1));f.drain();
    check(f.error_while_pending&&f.host.statistics().timeout_retry_rounds>0&&!f.host.workload_success(),"timeout exhausts once at logical level");f.save();
}
void dependency_cancel() {
    auto c=configuration();c.host.max_timeout_retries=0;Fixture f(c);f.start();
    auto dependent=read(3,0,64,Tick{100000000});dependent.after={GlobalRequestId{7}};
    f.submit(std::vector<GlobalRequest>{write(7,0,64),dependent});f.drain();
    check(f.events.now()<Tick{100000000}&&f.host.results().at(GlobalRequestId{3}).state==LogicalResult::Failed,"cancel obsolete future arrival");
    f.submit(write(1,32768,4096,f.events.now()));f.drain();
    check(f.host.results().at(GlobalRequestId{1}).state==LogicalResult::Success&&!f.host.workload_success(),"past failure retained after later success");f.save();
}
void recovery_failure() {
    auto c=configuration();c.faults.persistent_page=0;c.host.max_replay_passes=1;Fixture f(c);f.start();
    f.submit(std::vector<GlobalRequest>{write(9,0,8192),write(2,0,4096,{},2)});f.drain();
    check(f.host.statistics().replay_passes>0&&!f.host.workload_success(),"recovery exhaustion aborts old and subsequent content");
    for(const auto& r:f.host.results())check(r.second.state==LogicalResult::Failed,"all aborted Globals final");f.save();
}
void history() {
    Fixture f;f.start();f.submit(write(1000));f.drain();
    for(std::uint64_t id=1;id<=128;++id) {
        auto r=read(id,0,64,f.events.now());r.after={GlobalRequestId{1000}};f.submit(r);f.drain();
        check(f.host.storage().work==0&&f.host.storage().units==0&&f.host.storage().requests==0,"detailed objects reclaimed");
    }
    check(f.host.results().size()==129&&f.host.workload_success(),"history summaries retained with constant-time queries");
    rejects([&]{f.host.submit(read(1000,0,64,f.events.now()));});f.oracle();f.save();
}
void burst_finite() {
    auto c=configuration();c.host.writes=WriteCommandMode::Burst4096;Fixture f(c);
    auto r=read(20,0,4096);r.after={GlobalRequestId{30},GlobalRequestId{10}};
    f.host.load({write(30,0,2048),write(10,2048,2048),r});f.oracle();
    check(f.host.activity().unfinished_global==3,"three Globals despite one cross-Global write burst");
    f.host.start();f.drain();check(f.host.workload_success()&&f.host.statistics().handed==65,"finite burst completion accounting");f.save();
}
}
int main(int argc,char** argv) {
    try {
        const std::map<std::string,void(*)()> cases={{"empty",empty},{"partial",partial},{"admission",admission},
            {"future",future},{"feedback",feedback},{"replay",replay},{"pending_retry",pending_retry},
            {"timeout_retry",timeout_retry},{"dependency_cancel",dependency_cancel},{"recovery_failure",recovery_failure},
            {"history",history},{"burst_finite",burst_finite}};
        if(argc!=2&&(argc!=4||std::string(argv[2])!="--snapshot"))throw std::invalid_argument("scenario [--snapshot FILE]");
        cases.at(argv[1])();
        if(argc==4) {
            std::ofstream out(argv[3],std::ios::binary);out<<snapshot;out.close();
            check(bool(out),"snapshot output failed");
        }
        std::cout<<"PASS host_completion "<<argv[1]<<'\n';return 0;
    }catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
