#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace {
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
template<class F>void rejects(F f,const char* text){try{f();}catch(const std::exception& e){check(std::string(e.what()).find(text)!=std::string::npos,e.what());return;}throw std::runtime_error("missing rejection");}
SimulationConfiguration config(std::size_t limit=64){SimulationConfiguration c;c.device.geometry.host_channels=1;c.host.max_resident_fragments=limit;return c;}
GlobalRequest wr(std::uint64_t id,std::uint64_t addr=0,std::uint64_t bytes=4096,std::uint64_t content=1,Tick at={}){
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{addr},bytes,content};r.not_before=at;return r;
}
GlobalRequest rd(std::uint64_t id,Tick at={},std::uint64_t bytes=64){GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{},bytes};r.not_before=at;r.expected_content=1;return r;}
struct Fixture {
    StatisticsCollector stats;SimulationSession s;
    Fixture(SimulationConfiguration c=config()):s(resolve_configuration(c),&stats){}
    void idle(Tick t){auto r=s.advance_before(t);check(r.run.status==RunStatus::WaitingForInput,r.run.explanation.c_str());}
    void done(bool success=true){s.close_input();auto r=s.run_to_completion();check(r.run.status==RunStatus::Completed,r.run.explanation.c_str());check(r.run.workload_success==success,"logical outcome");}
    void reclaimed(){auto n=s.host().storage();check(!n.work&&!n.units&&!n.requests&&!n.attempts,"detailed records reclaimed");check(s.input_capacity().resident==0,"resident quota returned");}
};
void capacity() {
    Fixture f;auto a=f.s.try_submit(wr(1));check(a.disposition==InputDisposition::Registered&&a.capacity.resident==64,"count logical fragments");
    const auto events=f.s.next_event_time();auto b=f.s.try_submit(rd(2));
    check(b.disposition==InputDisposition::Backpressured&&b.capacity.resident==64,"temporary capacity failure");
    check(f.s.host().results().size()==1&&f.stats.globals().size()==1&&events==f.s.next_event_time(),"no rejected side effects");
    f.idle(Tick{2000000});f.reclaimed();check(f.s.input_capacity().generation>0,"capacity progress observable");
    auto r=rd(2,f.s.now());check(f.s.try_submit(r).disposition==InputDisposition::Registered,"rejected ID reusable");f.done();f.reclaimed();
}
void oversized() {
    Fixture f;auto r=f.s.try_submit(wr(1,0,8192));
    check(r.disposition==InputDisposition::TooLarge&&r.fragments==128&&!f.s.next_event_time(),"oversized batch never starts Host");
    f.s.set_completion_handler([](const auto&){});f.s.submit(wr(1));f.done();
}
void atomic_seal() {
    Fixture f;f.s.submit(wr(1,0,2048));
    auto r=f.s.try_submit(wr(2,0,4096,2));check(r.disposition==InputDisposition::Backpressured,"new content rejected for capacity");
    f.s.submit(wr(2,2048,2048));f.done();check(f.s.host().results().size()==2,"old content not sealed by rejected batch");
}
void invalid_full() {
    Fixture f;f.s.submit(wr(1));
    rejects([&]{f.s.try_submit(wr(1));},"duplicate");
    rejects([&]{f.s.try_submit(wr(2,0,64));},"duplicate write fragment");
    check(f.s.input_capacity().resident==64,"bad input unchanged");f.done();
}
void future_budget() {
    Fixture f;f.s.submit(wr(1,0,4096,1,Tick{1000}));
    check(f.s.try_submit(rd(2)).disposition==InputDisposition::Backpressured,"future work consumes resident capacity");
    f.s.advance_before(Tick{1000});check(!f.s.host().statistics().handed&&f.s.input_capacity().resident==64,"no implicit send slot");f.done();
}
void burst_budget() {
    auto c=config();c.host.writes=WriteCommandMode::Burst4096;Fixture f(c);
    f.s.submit(std::vector<GlobalRequest>{wr(1,0,2048),wr(2,2048,2048)});
    check(f.s.input_capacity().resident==64,"burst costs 64 logical fragments");f.done();f.reclaimed();
    check(f.s.host().statistics().handed==1&&f.s.host().results().size()==2,"one command two globals");
}
void strict_timeout() {
    auto c=config(32);c.host.max_timeout_retries=0;Fixture f(c);
    f.s.submit(wr(1,0,2048));check(f.s.try_submit(wr(2,2048,2048)).disposition==InputDisposition::Backpressured,"strict budget has no completion reserve");
    f.done(false);f.reclaimed();check(f.s.host().statistics().assembly_timeout_errors==32,"timeout drains accepted prefix");
}
void strict_stall() {
    Fixture f;f.s.submit(wr(1,4096));check(f.s.try_submit(wr(2)).disposition==InputDisposition::Backpressured,"predecessor excluded by strict budget");
    auto r=f.s.advance_before(Tick{100});check(r.run.status==RunStatus::WaitingForInput&&r.input_capacity.resident==64,"open blocked input reported");
    f.s.close_input();check(f.s.run_to_completion().run.status==RunStatus::Stalled,"no fabricated timeout or capacity");
}
void summaries() {
    Fixture f;f.s.submit(wr(1));f.idle(Tick{2000000});f.reclaimed();
    rejects([&]{f.s.submit(wr(1,4096,4096,1,f.s.now()));},"duplicate Global");
    rejects([&]{f.s.submit(wr(2,0,64,1,f.s.now()));},"duplicate write fragment");
    auto r=rd(2,f.s.now());r.after={GlobalRequestId{1}};f.s.submit(r);f.idle(Tick{3000000});f.reclaimed();
    f.s.submit(wr(3,4096,4096,1,f.s.now()));f.done();f.reclaimed();
    check(f.s.host().results().size()==3&&f.s.device().statistics().programs==2,"dependency and predecessor survive collection");
}
void sealed_summary() {
    Fixture f;f.s.submit(wr(1));f.idle(Tick{2000000});f.s.submit(wr(2,0,4096,9,f.s.now()));f.idle(Tick{4000000});f.reclaimed();
    rejects([&]{f.s.submit(wr(3,4096,4096,1,f.s.now()));},"sealed");f.done();
}
void failed_summary() {
    auto c=config();c.host.max_timeout_retries=0;Fixture f(c);f.s.submit(wr(1,0,64));f.idle(Tick{20000000});f.reclaimed();
    auto r=rd(2,f.s.now());r.after={GlobalRequestId{1}};f.s.submit(r);f.done(false);f.reclaimed();
    check(f.s.host().results().at(GlobalRequestId{2}).reason=="DependencyFailed","failed dependency preserved");
}
void replay_collected() {
    auto c=config();c.faults.fail_sequence={1};Fixture f(c);
    f.s.submit(wr(1));f.idle(Tick{2000000});f.reclaimed();check(f.s.host().retained().size()==64,"recovery data independent of work objects");
    f.s.submit(wr(2,4096,4096,1,f.s.now()));f.done();f.reclaimed();
    check(f.s.host().statistics().replay_commands==128&&f.s.host().statistics().logical_success==128,"replay reconstructs collected page without double completion");
}
void replay_merged() {
    auto c=config();c.faults.fail_sequence={2,1};Fixture f(c);
    f.s.submit(wr(1));f.idle(Tick{2000000});f.reclaimed();
    f.s.submit(wr(2,4096,4096,1,f.s.now()));f.idle(Tick{4000000});f.reclaimed();
    f.s.submit(wr(3,8192,4096,1,f.s.now()));f.done();f.reclaimed();
    check(f.s.host().statistics().recovery_tasks==1&&f.s.host().statistics().replay_passes==2,"merged target survives retired ordinary work");
}
void missing_retention() {
    auto c=config();c.faults.fail_sequence={1};c.host.retained_fragments=0;Fixture f(c);
    f.s.submit(wr(1));f.idle(Tick{2000000});f.reclaimed();f.s.submit(wr(2,4096,4096,1,f.s.now()));f.done(false);f.reclaimed();
    check(f.s.host().results().at(GlobalRequestId{1}).state==LogicalResult::Success,"past success not rewritten by missing recovery data");
    check(f.s.host().results().at(GlobalRequestId{2}).reason=="MissingRecoveryData","never restore from device private state");
}
void pending_retry() {
    auto c=config(128);c.device.capabilities.max_pending_write_dlus=1;Fixture f(c);
    f.s.submit(std::vector<GlobalRequest>{wr(1),wr(2,32768)});f.done();f.reclaimed();
    check(f.s.host().statistics().pending_limit_errors>0&&f.s.host().statistics().pending_retry_commands>0,"Done retry units stay live");
}
void callback_retry() {
    Fixture f;bool pending=false;unsigned capacity_calls=0;
    f.s.set_completion_handler([&](const GlobalCompletion& r){if(r.result.id.value==1){pending=true;}});
    f.s.set_input_capacity_handler([&](InputCapacity c){
        ++capacity_calls;check(c.generation&&f.s.input_capacity().generation==c.generation,"current capacity generation");
        if(pending) {auto r=rd(2,f.s.now(),4096);r.offered_at=Tick{};
            if(f.s.try_submit_feedback(r).disposition==InputDisposition::Registered)pending=false;}
    });
    f.s.submit(wr(1));f.done();f.reclaimed();
    check(!pending&&capacity_calls&&f.s.host().results().size()==2,"closed-source feedback retries on capacity event");
    check(f.stats.globals().at(GlobalRequestId{2}).offered==Tick{},"producer origin retained");
}
void timeout_retry() {
    Fixture f;f.s.submit(std::vector<GlobalRequest>{wr(1,0,2048),wr(2,2048,2048,1,Tick{12000000})});
    auto waiting=f.s.advance_before(Tick{11000000});
    check(waiting.input_capacity.resident==64&&f.s.host().statistics().assembly_timeout_errors==32,"timed-out work retains quota pending retry");
    f.done();f.reclaimed();check(f.s.host().statistics().timeout_retry_commands==32,"only previous attempts counted as retransmission");
}
void callback_backpressure() {
    Fixture f(config(128));bool pending=false;unsigned blocked=0;
    const auto retry=[&] {
        if(!pending)return;
        auto r=wr(3,65536,8192,1,f.s.now());r.offered_at=Tick{};
        auto a=f.s.try_submit_feedback(r);
        if(a.disposition==InputDisposition::Backpressured)++blocked;
        else {check(a.disposition==InputDisposition::Registered,"feedback fits total budget");pending=false;}
    };
    f.s.set_completion_handler([&](const GlobalCompletion& e){if(e.result.id.value==1){pending=true;retry();}});
    f.s.set_input_capacity_handler([&](InputCapacity){retry();});
    f.s.submit(std::vector<GlobalRequest>{wr(1),wr(2,32768,4096,1,Tick{5000000})});f.done();f.reclaimed();
    check(blocked&&!pending&&f.s.host().results().size()==3,"feedback retained across capacity rejections after source close");
}
void offered_time() {
    Fixture f;f.s.submit(wr(1));auto r=rd(2,{},4096);r.offered_at=Tick{};
    check(f.s.try_submit(r).disposition==InputDisposition::Backpressured,"source input delayed");f.idle(Tick{2000000});
    rejects([&]{f.s.try_submit(r);},"boundary");r.not_before=f.s.now();f.s.submit(r);f.done();
    const auto& g=f.stats.globals().at(GlobalRequestId{2});check(g.registered==Tick{2000000}&&g.offered==Tick{},"original offer distinct from registration/release");
    auto stats=f.stats.summarize(f.s.now());check(stats.latency.at("read.global.success.input_wait").mean==2000000,"input wait visible");
    check(stats.latency.at("read.global.success.source_end_to_end").min==g.finished->value,"source end-to-end includes input waiting");
}
void origin_invalid() {
    Fixture f;auto r=wr(1);r.offered_at=Tick{1};rejects([&]{f.s.try_submit(r);},"offer time");
    r.not_before=Tick{2};rejects([&]{f.s.try_submit(r);},"registration time");check(!f.s.next_event_time(),"invalid metadata atomic");
}
void retained_failure() {
    auto c=config(128);c.host.max_timeout_retries=0;Fixture f(c);f.s.submit(wr(1,0,64));
    // Future failed work is removed while another request still has a release.
    f.s.submit(wr(2,4096,64,1,Tick{30000000}));auto r=rd(3,Tick{40000000});r.after={GlobalRequestId{1}};f.s.submit(r);
    f.done(false);f.reclaimed();check(f.s.host().results().size()==3,"future indices never dereference collected units");
}
void long_input() {
    auto c=config();c.host.max_logical_fragments=0;Fixture f(c);f.s.submit(wr(1));f.idle(Tick{2000000});
    for(std::uint64_t id=2;id<102;++id){f.s.submit(rd(id,f.s.now()));f.idle(Tick{f.s.now().value+200000});f.reclaimed();}
    f.done();check(f.s.host().storage().results==101&&f.s.host().storage().versions==1,"retain compact identities, not execution histories");
}
void lifetime_limit() {
    auto c=config();c.host.max_logical_fragments=64;Fixture f(c);f.s.submit(wr(1));f.idle(Tick{2000000});f.reclaimed();
    rejects([&]{f.s.try_submit(rd(2,f.s.now()));},"fragment limit");f.done();
}
void configuration() {
    auto c=config();apply_configuration_override(c,"host.max_resident_fragments","128");apply_configuration_override(c,"host.max_logical_fragments","0");
    apply_configuration_override(c,"output.statistics_buffer_bytes","97");apply_configuration_override(c,"output.statistics_events_jsonl","events.jsonl");
    auto resolved=resolve_configuration(c);std::ostringstream out;write_effective_configuration(out,resolved);std::istringstream in(out.str());
    auto again=resolve_configuration(read_configuration(in));check(again.values().host.max_resident_fragments==128&&again.values().host.max_logical_fragments==0,"capacity configuration roundtrip");
    check(again.values().output.statistics_buffer_bytes==97,"stream configuration roundtrip");c.output.statistics_buffer_bytes=0;rejects([&]{resolve_configuration(c);},"positive");
}
}
int main(int argc,char** argv){
    const std::map<std::string,void(*)()> tests={{"capacity",capacity},{"oversized",oversized},{"atomic_seal",atomic_seal},{"invalid_full",invalid_full},
        {"future_budget",future_budget},{"burst_budget",burst_budget},{"strict_timeout",strict_timeout},{"strict_stall",strict_stall},
        {"summaries",summaries},{"sealed_summary",sealed_summary},{"failed_summary",failed_summary},{"replay_collected",replay_collected},
        {"replay_merged",replay_merged},{"missing_retention",missing_retention},{"pending_retry",pending_retry},{"callback_retry",callback_retry},
        {"offered_time",offered_time},{"origin_invalid",origin_invalid},{"retained_failure",retained_failure},{"long_input",long_input},
        {"lifetime_limit",lifetime_limit},{"configuration",configuration},{"timeout_retry",timeout_retry},{"callback_backpressure",callback_backpressure}};
    try{if(argc!=2)throw std::runtime_error("scenario required");tests.at(argv[1])();std::cout<<"PASS resident "<<argv[1]<<'\n';}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
