#include "hbf_eventlab/runtime/simulation_session.hpp"
#include <iostream>
using namespace hbf_eventlab;
namespace {
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("missing rejection");}
SimulationConfiguration config(){SimulationConfiguration c;c.device.geometry.host_channels=1;return c;}
GlobalRequest write(std::uint64_t id,std::uint64_t address=0,std::uint64_t bytes=4096,Tick at={}){
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{address},bytes,1};r.not_before=at;return r;
}
void attach(SimulationSession& s){s.set_completion_handler([&](const auto&){s.request_yield();});}
void handoff() {
    SimulationSession s(resolve_configuration(config()));Tick completed;
    s.set_completion_handler([&](const auto& e){completed=e.at;s.request_yield();});s.submit(write(1));
    auto r=s.advance_until_yield();check(r.feedback_yield&&r.run.status==RunStatus::Paused&&r.run.at==completed,"completion yields at its own tick");
    check(s.can_resume_feedback()&&!s.accepts_at_floor(),"causal permit does not reopen external boundary");
    s.close_input();check(s.run_to_completion().run.status==RunStatus::Completed,"drain after handoff");
}
void zero_compute() {
    SimulationSession s(resolve_configuration(config()));attach(s);s.submit(write(1));s.advance_until_yield();const auto t=s.now();
    rejects([&]{s.submit(write(2,32768,4096,t));});bool ran=false;
    s.resume_feedback([&]{check(s.now()==t,"continuation stays at tick");ran=true;s.submit_feedback(write(2,32768,4096,t));});
    check(!ran,"continuation is event driven");auto r=s.advance_until_yield();check(ran&&r.feedback_yield&&s.host().results().size()==2,"same tick submission succeeds causally");
}
void future_no_skip() {
    SimulationSession s(resolve_configuration(config()));attach(s);s.submit(std::vector<GlobalRequest>{write(1),write(2,32768,64,Tick{100000000})});
    auto r=s.advance_until_yield();check(r.run.at<Tick{100000000}&&s.host().results().at(GlobalRequestId{2}).state==LogicalResult::Pending,"do not run future arrivals/deadlines past response");
}
void guards() {
    SimulationSession s(resolve_configuration(config()));rejects([&]{s.request_yield();});rejects([&]{s.resume_feedback([]{});});
    attach(s);s.submit(write(1));s.advance_until_yield();rejects([&]{s.resume_feedback({});});check(s.can_resume_feedback(),"empty callback preserves permit");
}
void one_shot() {
    SimulationSession s(resolve_configuration(config()));attach(s);s.submit(write(1));s.advance_until_yield();
    bool ran=false;s.resume_feedback([&]{ran=true;s.request_yield();});rejects([&]{s.resume_feedback([]{});});
    const auto t=s.now();check(s.advance_until_yield().feedback_yield&&ran&&s.now()==t,"one-shot continuation can yield again");
}
void expired() {
    SimulationSession s(resolve_configuration(config()));attach(s);s.submit(write(1));s.advance_until_yield();
    s.advance_before(Tick{s.now().value+1});rejects([&]{s.resume_feedback([]{});});
}
void closed() {
    SimulationSession s(resolve_configuration(config()));attach(s);s.submit(write(1));s.close_input();
    check(s.run_to_completion().feedback_yield,"explicit callback pause applies while draining");
    s.resume_feedback([&]{s.submit_feedback(write(2,32768,4096,s.now()));});check(s.run_to_completion().feedback_yield,"causal feedback may follow source closure");
    check(s.run_to_completion().run.status==RunStatus::Completed,"closure plus feedback drains");
}
void capacity() {
    auto c=config();c.host.max_resident_fragments=64;SimulationSession s(resolve_configuration(c));
    s.set_input_capacity_handler([&](InputCapacity available){check(available.resident<available.limit,"capacity really reclaimed");s.request_yield();});
    s.submit(write(1));check(s.advance_until_yield().feedback_yield,"capacity event supports handoff");
}
void error() {
    SimulationSession s(resolve_configuration(config()));s.set_completion_handler([&](const auto&){s.request_yield();throw std::runtime_error("failure after yield");});s.submit(write(1));
    check(s.advance_until_yield().run.status==RunStatus::ImplementationError&&!s.can_resume_feedback(),"callback exception overrides yield");
}
void total_limit() {
    SimulationSession s(resolve_configuration(config()));attach(s);s.submit(write(1));auto first=s.advance_until_yield();
    auto limits=s.run_limits();limits.events.total_events=first.run.events;s.set_run_limits(limits);
    s.resume_feedback([&]{s.request_yield();});check(s.advance_until_yield().run.status==RunStatus::LimitReached,"yield does not reset cumulative event budget");
}
void deadline() {
    auto c=config();c.host.max_timeout_retries=0;SimulationSession s(resolve_configuration(c));std::uint64_t calls=0;
    s.set_completion_handler([&](const auto& e){++calls;check(e.result.state==LogicalResult::Failed,"timeout terminal");s.request_yield();});
    s.submit(std::vector<GlobalRequest>{write(1,0,64),write(2,32768,64)});
    auto r=s.advance_until_yield();check(r.feedback_yield&&calls==1,"timeout feedback yields");
    bool resumed=false;s.resume_feedback([&]{resumed=true;rejects([&]{s.submit_feedback(write(3,64,4032,s.now()));});s.request_yield();});
    while(!resumed){r=s.advance_until_yield();check(r.feedback_yield,"remaining feedback or continuation");}
    check(s.host().results().at(GlobalRequestId{1}).state==LogicalResult::Failed,"late continuation cannot rescue timeout");
}
void reentrant() {
    SimulationSession s(resolve_configuration(config()));s.set_completion_handler([&](const auto&){rejects([&]{s.advance_until_yield();});rejects([&]{s.resume_feedback([]{});});s.request_yield();});
    s.submit(write(1));check(s.advance_until_yield().feedback_yield,"recursive drive rejected without corrupting callback");
}
void empty() {
    SimulationSession s(resolve_configuration(config()));const auto r=s.advance_until_yield();
    check(r.run.status==RunStatus::WaitingForInput&&s.now()==Tick{}&&!s.can_resume_feedback(),"empty open input neither complete nor time jump");
    s.close_input();check(s.advance_until_yield().run.status==RunStatus::Completed,"empty closed completion");
}
}
int main(int argc,char** argv) {
    const std::map<std::string,void(*)()> tests={{"handoff",handoff},{"zero_compute",zero_compute},{"future_no_skip",future_no_skip},{"guards",guards},
        {"one_shot",one_shot},{"expired",expired},{"closed",closed},{"capacity",capacity},{"error",error},{"total_limit",total_limit},{"deadline",deadline},{"reentrant",reentrant},{"empty",empty}};
    try{if(argc!=2)throw std::runtime_error("scenario required");tests.at(argv[1])();std::cout<<"PASS session_handoff "<<argv[1]<<'\n';}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
