#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <iostream>
#include <sstream>

using namespace hbf_eventlab;
namespace {
void check(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
template<class F> void rejects(F fn,const std::string& text) {
    try {fn();} catch(const std::exception& e) {check(std::string(e.what()).find(text)!=std::string::npos,e.what());return;}
    throw std::runtime_error("missing rejection: "+text);
}
SimulationConfiguration config() {SimulationConfiguration c;c.device.geometry.host_channels=1;return c;}
GlobalRequest write(std::uint64_t id,std::uint64_t address=0,std::uint64_t bytes=4096,Tick at={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{address},bytes,1};r.not_before=at;return r;
}
GlobalRequest read(std::uint64_t id,Tick at={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{},64};r.not_before=at;r.expected_content=1;return r;
}
struct Fixture {
    StatisticsCollector stats;
    SimulationSession s;
    Fixture(SimulationConfiguration c=config()):s(resolve_configuration(c),&stats) {}
    SessionReport finish() {s.close_input();auto r=s.run_to_completion();check(r.run.status==RunStatus::Completed,"completed");check(r.run.host.drained()&&r.run.device.drained(),"both owners drained");return r;}
    std::string json() {std::ostringstream out;stats.write_json(out,s.now(),"Drained");return out.str();}
};
void exclusive() {
    Fixture f;f.s.submit(write(1,0,4096,Tick{100}));
    auto r=f.s.advance_before(Tick{100});
    check(r.run.status==RunStatus::Paused&&r.run.at==Tick{100}&&r.accepts_at_floor,"exclusive endpoint remains open");
    check(!f.s.host().statistics().handed&&!f.stats.globals().at(GlobalRequestId{1}).released,"endpoint event not executed");
    check(f.finish().run.workload_success,"future request completes");
}
void empty_frontier() {
    Fixture f;auto r=f.s.advance_before(Tick{100});
    check(r.run.status==RunStatus::WaitingForInput&&r.input_floor==Tick{100}&&r.accepts_at_floor,"empty interval committed");
    const auto snapshot=f.json();
    rejects([&]{f.s.submit(write(1,0,4096,Tick{99}));},"boundary");
    check(f.json()==snapshot&&!f.s.next_event_time(),"late input no mutation");
    f.s.submit(write(1,0,4096,Tick{100}));check(f.finish().run.workload_success,"endpoint input permitted");
}
void endpoint_input() {
    Fixture f;f.s.submit(write(2,4096,4096,Tick{200}));
    f.s.advance_before(Tick{100});f.s.submit(write(1,0,4096,Tick{100}));
    check(f.finish().run.workload_success&&f.s.device().statistics().programs==2,"earlier input fills predecessor at boundary");
}
void first_atomic() {
    Fixture f;rejects([&]{f.s.submit(std::vector<GlobalRequest>{write(1),write(2,4096,63)});},"unaligned");
    check(!f.s.next_event_time()&&f.stats.globals().empty()&&f.s.host().results().empty(),"initial rejection has no startup side effects");
    f.s.set_completion_handler([](const auto&){});f.s.submit(write(1));check(f.finish().run.workload_success,"ID and setup remain reusable");
}
void late_atomic() {
    Fixture f;f.s.advance_before(Tick{100});
    rejects([&]{f.s.submit(std::vector<GlobalRequest>{write(1,0,4096,Tick{100}),write(2,4096,4096,Tick{99})});},"boundary");
    check(f.s.host().results().empty()&&!f.s.next_event_time(),"late member rejects entire batch");
}
void horizon_guard() {
    Fixture f;auto a=f.s.advance_before(Tick{100});
    rejects([&]{f.s.advance_before(Tick{99});},"past");
    auto b=f.s.advance_before(Tick{100});check(a.run.events==b.run.events&&b.accepts_at_floor,"repeat open boundary adds no events");
    rejects([&]{f.s.run_to_completion();},"closed external input");
}
void open_drained() {
    Fixture f;f.s.submit(write(1));auto r=f.s.advance_before(Tick{10000000});
    check(r.run.status==RunStatus::WaitingForInput&&!r.input_closed&&r.run.host.drained()&&r.run.device.drained(),"drained is not input EOF");
    f.s.submit(read(2,f.s.now()));check(f.finish().run.workload_success,"persistent media survives waiting");
}
void close_work() {
    Fixture f;f.s.submit(write(1,0,4096,Tick{100000}));f.s.close_input();f.s.close_input();
    rejects([&]{f.s.submit(write(2,4096));},"closed");
    check(f.s.next_event_time().has_value()&&f.finish().run.workload_success,"close preserves registered future work");
}
void close_empty() {
    Fixture f;auto a=f.finish();auto b=f.s.run_to_completion();
    check(a.run.workload_success&&a.run.events==b.run.events&&b.run.status==RunStatus::Completed,"empty closed run and repeat are stable");
}
void open_timeout() {
    auto c=config();c.host.max_timeout_retries=0;Fixture f(c);f.s.submit(write(1,0,64));
    auto early=f.s.advance_before(Tick{2000000});
    check(early.run.status==RunStatus::Paused&&early.next_event&&early.run.host.unfinished_global,"future timeout is progress");
    auto late=f.s.advance_before(Tick{20000000});
    check(late.run.status==RunStatus::WaitingForInput&&!late.run.workload_success&&late.run.device.drained(),"timeout runs while input open");
    check(f.finish().run.status==RunStatus::Completed,"logical failure still drains");
}
void closed_stalled() {
    Fixture f;f.s.submit(write(1,4096));f.s.close_input();auto r=f.s.run_to_completion();
    check(r.run.status==RunStatus::Stalled&&r.run.host.unfinished_global&&!r.next_event,"closed missing predecessor stalls");
}
void missing_predecessor() {
    Fixture f;f.s.submit(write(1,4096));auto r=f.s.advance_before(Tick{100});
    check(r.run.status==RunStatus::WaitingForInput&&r.run.host.unfinished_global,"open dependency can wait for input");
    f.s.submit(write(2,0,4096,f.s.now()));check(f.finish().run.workload_success,"later input resolves dependency");
}
void feedback_closed() {
    Fixture f;std::vector<GlobalRequestId> done;
    f.s.set_completion_handler([&](const GlobalCompletion& c){
        done.push_back(c.result.id);check(c.at==f.s.now()&&f.s.input_closed(),"same tick callback after close");
        if(c.result.id.value==1)f.s.submit_feedback(write(2,4096,4096,f.s.now()));
        if(c.result.id.value==2)f.s.submit_feedback(read(3,f.s.now()));
    });
    f.s.submit(write(1));check(done.empty(),"callback is asynchronous");
    check(f.finish().run.workload_success&&done.size()==3,"closed external source permits internal feedback chain");
}
void feedback_guard() {
    Fixture f;rejects([&]{f.s.submit_feedback(read(2));},"feedback event");
    f.s.set_completion_handler([&](const GlobalCompletion& c){
        rejects([&]{f.s.submit(read(2,f.s.now()));},"not reentrant");
        rejects([&]{f.s.advance_before(Tick{2000000});},"not reentrant");
        rejects([&]{f.s.close_input();},"not reentrant");
        rejects([&]{f.s.set_run_limits({});},"not reentrant");
        if(c.result.id.value==1)f.s.submit_feedback(read(2,f.s.now()));
    });
    f.s.submit(write(1));check(f.finish().run.workload_success,"feedback entry only inside callback");
    rejects([&]{f.s.submit_feedback(read(3,f.s.now()));},"feedback event");
}
void feedback_future() {
    Fixture f;f.s.set_completion_handler([&](const GlobalCompletion& c){if(c.result.id.value==1)f.s.submit_feedback(read(2,Tick{10000000}));});
    f.s.submit(write(1));f.s.close_input();auto r=f.s.advance_before(Tick{5000000});
    check(r.run.status==RunStatus::Paused&&r.next_event==Tick{10000000}&&r.run.host.unfinished_global==1,"registered future feedback prevents early completion");
    check(f.finish().run.workload_success,"future feedback delivered");
}
void feedback_validation() {
    Fixture f;f.s.set_completion_handler([&](const GlobalCompletion& c){if(c.result.id.value==1){
        rejects([&]{f.s.submit_feedback(std::vector<GlobalRequest>{read(2,f.s.now()),read(1,f.s.now())});},"duplicate");
        check(f.s.host().results().size()==1,"invalid feedback batch rolled back");f.s.submit_feedback(read(2,f.s.now()));
    }});
    f.s.submit(write(1));check(f.finish().run.workload_success,"caught feedback input error is recoverable");
}
void callback_error() {
    Fixture f;f.s.set_completion_handler([](const auto&){throw std::runtime_error("callback failed");});
    f.s.submit(write(1));f.s.close_input();auto a=f.s.run_to_completion(),b=f.s.run_to_completion();
    check(a.run.status==RunStatus::ImplementationError&&b.run.events==a.run.events,"callback exception terminal, not replayed");
    rejects([&]{f.s.submit(read(2,f.s.now()));},"terminal");
}
void callback_model_error() {
    Fixture f;f.s.set_completion_handler([&](const auto&){f.s.submit_feedback(read(1,f.s.now()));});
    f.s.submit(write(1));f.s.close_input();check(f.s.run_to_completion().run.status==RunStatus::ModelError,"uncaught feedback contract error is diagnosed");
}
void limit_case(const std::string& kind) {
    auto c=config();
    if(kind=="total")c.run.events.total_events=1;
    if(kind=="tick")c.run.events.events_per_tick=1;
    if(kind=="round")c.run.events.rounds_per_tick=1;
    Fixture f(c);f.s.submit(write(1));auto a=f.s.advance_before(Tick{10000000});
    check(a.run.status==RunStatus::LimitReached,"safety budget triggers");
    auto b=f.s.advance_before(Tick{10000000});
    check(b.run.status==RunStatus::LimitReached&&a.run.events==b.run.events&&a.run.at==b.run.at,"pause does not reset safety budget");
    auto limits=config().run;f.s.set_run_limits(limits);check(f.finish().run.workload_success,"explicit increase resumes retained work");
}
void limit_total(){limit_case("total");} void limit_tick(){limit_case("tick");} void limit_round(){limit_case("round");}
void time_empty() {
    auto c=config();c.run.max_tick=Tick{50};Fixture f(c);
    auto a=f.s.advance_before(Tick{100});check(a.run.status==RunStatus::LimitReached&&a.run.at==Tick{50},"empty interval obeys time safety ceiling");
    auto b=f.s.advance_before(Tick{100});check(b.run.events==a.run.events&&b.run.status==RunStatus::LimitReached,"time limit persists");
    f.s.set_run_limits(config().run);check(f.s.advance_before(Tick{100}).run.status==RunStatus::WaitingForInput,"explicit time ceiling increase");
    rejects([&]{f.s.set_run_limits(c.run);},"before the current clock");
}
void time_future() {
    auto c=config();c.run.max_tick=Tick{50};Fixture f(c);f.s.submit(write(1,0,4096,Tick{200}));
    auto r=f.s.advance_before(Tick{100});check(r.run.status==RunStatus::LimitReached&&r.run.at==Tick{50}&&!f.s.host().statistics().handed,"future request not executed across ceiling");
    f.s.set_run_limits(config().run);check(f.finish().run.workload_success,"future event retained");
}
void entered_tick() {
    auto c=config();c.run.events.total_events=1;Fixture f(c);f.s.submit(write(1));
    auto r=f.s.advance_before(Tick{10000000});check(!r.accepts_at_floor,"partially executed tick locked to external producer");
    rejects([&]{f.s.submit(write(2,32768,4096,r.input_floor));},"entered tick");
    rejects([&]{f.s.advance_before(r.input_floor);},"already entered");
    f.s.submit(write(2,32768,4096,Tick{r.input_floor.value+1}));
    f.s.set_run_limits(config().run);check(f.finish().run.workload_success,"future external input still allowed after safety pause");
}
void equivalent() {
    auto c=config();auto resolved=resolve_configuration(c);StatisticsCollector stats;EventQueue e;
    DeviceDriver d(resolved.description(),c.execution,e);RoundRobinInterleaver inter({},1,resolved.description().geometry().channel_bytes());
    HostDriver h(d,e,inter,c.host);d.set_statistics_sink(stats);h.set_statistics_sink(stats);
    auto r=read(3,Tick{200000});r.after={GlobalRequestId{1},GlobalRequestId{2}};
    std::vector<GlobalRequest> input{write(1),write(2,4096,4096,Tick{100000}),r};
    h.load(input);h.start();SimulationRunner runner(e,h,d);auto offline=runner.run();
    Fixture bounded(c);bounded.s.submit(input);bounded.s.advance_before(Tick{1000});bounded.s.advance_before(Tick{150000});auto done=bounded.finish();
    std::ostringstream json;stats.write_json(json,e.now(),"Drained");
    check(json.str()==bounded.json()&&offline.events==done.run.events,"pause/continue identical to finite run including histories and event count");
    Fixture incremental(c);incremental.s.submit(input[0]);incremental.s.advance_before(Tick{100000});incremental.s.submit(input[1]);
    incremental.s.advance_before(Tick{200000});incremental.s.submit(input[2]);incremental.finish();
    check(json.str()==incremental.json(),"just-in-time boundary submissions preserve physical results and statistics");
}
void causal_time() {
    Fixture f;Tick delivered;
    f.s.set_completion_handler([&](const GlobalCompletion& c){
        check(f.s.input_floor()==f.s.now()&&!f.s.accepts_at_floor(),"callback boundary view matches current event");
        if(c.result.id.value==1){delivered=c.at;f.s.submit_feedback(read(2,c.at));}
    });
    f.s.submit(write(1));f.finish();
    check(f.stats.globals().at(GlobalRequestId{2}).released==delivered,"feedback does not fabricate a tick");
}
void recovery() {
    auto c=config();c.faults.fail_sequence={0};Fixture f(c);unsigned completed=0;
    f.s.set_completion_handler([&](const auto&){++completed;});f.s.submit(write(1));
    f.s.advance_before(Tick{1600000});check(f.s.host().statistics().recovery_tasks==1,"recovery active across pause");
    check(f.finish().run.workload_success&&f.s.host().statistics().replay_passes==1&&completed==1,"same Host recovery, one logical completion");
}
void clock_idle() {
    EventQueue q;bool fired=false;auto cancelled=q.schedule_at(Tick{5},EventPhase::Arrival,[]{});q.cancel(cancelled);
    q.schedule_at(Tick{20},EventPhase::Arrival,[&]{fired=true;rejects([&]{q.advance_idle_to(Tick{20});},"during an event");});
    q.advance_idle_to(Tick{20});check(q.now()==Tick{20}&&!fired&&!q.tick_started()&&!q.executed(),"idle advance skips no event and consumes no budget");
    rejects([&]{q.advance_idle_to(Tick{21});},"skip a live event");rejects([&]{q.advance_idle_to(Tick{19});},"backwards");
    q.run();q.advance_idle_to(Tick{20});check(q.tick_started(),"same tick idle call cannot reopen executed tick");
    q.advance_idle_to(Tick{21});check(!q.tick_started()&&q.executed()==1,"new idle tick leaves lifetime event count intact");
}
void frozen_deadline() {
    EventQueue q;std::vector<int> order;q.set_limits({1,100,100});
    q.schedule_at(Tick{10},EventPhase::Deadline,[&]{order.push_back(1);q.schedule_at(q.now(),EventPhase::Arrival,[&]{order.push_back(3);});});
    q.schedule_at(Tick{10},EventPhase::Deadline,[&]{order.push_back(2);});
    rejects([&]{q.run_next();},"limit");check(q.tick_started()&&order.empty(),"frozen cohort closes tick even before first callback");
    rejects([&]{q.advance_idle_to(Tick{11});},"frozen deadline");q.set_limits({100,100,100});q.run_next();
    check(order==std::vector<int>({1,2}),"deadline cohort completes before feedback");q.run();check(order==std::vector<int>({1,2,3}),"same tick post-settlement feedback");
}
void deadline_endpoint() {
    auto c=config();c.host.max_timeout_retries=0;Fixture f(c);f.s.submit(write(1,0,64));f.s.advance_before(Tick{2000000});
    const auto deadline=*f.s.next_event_time();auto paused=f.s.advance_before(deadline);
    check(paused.run.status==RunStatus::Paused&&paused.accepts_at_floor,"deadline endpoint not settled yet");
    // Host release at the deadline is not Device acceptance at the deadline.
    f.s.submit(write(2,64,4032,deadline));auto end=f.finish();
    check(!end.run.workload_success&&f.s.host().statistics().assembly_timeout_errors,"link/RX delay cannot be bypassed to rescue expired assembly");
}
void deadline_feedback() {
    auto c=config();c.host.max_timeout_retries=0;Fixture f(c);bool observed=false;
    f.s.set_completion_handler([&](const GlobalCompletion& done){if(done.result.id.value==1){
        observed=true;check(done.result.state==LogicalResult::Failed,"timeout already final");
        rejects([&]{f.s.submit_feedback(write(2,64,4032,f.s.now()));},"already aborted");
        f.s.submit_feedback(write(2,32768,4096,f.s.now()));
    }});
    f.s.submit(write(1,0,64));auto r=f.finish();
    check(observed&&!r.run.workload_success&&f.s.host().results().at(GlobalRequestId{2}).state==LogicalResult::Success,"post-timeout feedback may start other work, never rescue old context");
}
void maximum_boundary() {
    auto c=config();c.run.max_tick.reset();Fixture f(c);const Tick last{UINT64_MAX};
    check(f.s.advance_before(Tick{}).run.status==RunStatus::Paused,"zero boundary is exclusive");
    auto r=f.s.advance_before(last);check(r.run.status==RunStatus::WaitingForInput&&r.input_floor==last&&r.accepts_at_floor,"frontier does not require tick+1 arithmetic");
    check(f.finish().run.status==RunStatus::Completed,"empty maximum-tick close");
}
}
int main(int argc,char** argv) {
    const std::map<std::string,void(*)()> tests={{"exclusive",exclusive},{"empty_frontier",empty_frontier},{"endpoint_input",endpoint_input},
        {"first_atomic",first_atomic},{"late_atomic",late_atomic},{"horizon_guard",horizon_guard},{"open_drained",open_drained},
        {"close_work",close_work},{"close_empty",close_empty},{"open_timeout",open_timeout},{"closed_stalled",closed_stalled},{"missing_predecessor",missing_predecessor},
        {"feedback_closed",feedback_closed},{"feedback_guard",feedback_guard},{"feedback_future",feedback_future},{"feedback_validation",feedback_validation},
        {"callback_error",callback_error},{"callback_model_error",callback_model_error},{"limit_total",limit_total},{"limit_tick",limit_tick},{"limit_round",limit_round},
        {"time_empty",time_empty},{"time_future",time_future},{"entered_tick",entered_tick},{"equivalent",equivalent},{"causal_time",causal_time},
        {"recovery",recovery},{"clock_idle",clock_idle},{"frozen_deadline",frozen_deadline},{"deadline_endpoint",deadline_endpoint},{"deadline_feedback",deadline_feedback},{"maximum_boundary",maximum_boundary}};
    try {if(argc!=2)throw std::runtime_error("scenario required");tests.at(argv[1])();std::cout<<"PASS session "<<argv[1]<<'\n';}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
