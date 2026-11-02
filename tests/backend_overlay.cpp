#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "host_index_oracle.hpp"
#include <iostream>

using namespace hbf_eventlab;
namespace {
void check(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F fn,const char* part) {
    try {fn();}catch(const std::exception& e){check(std::string(e.what()).find(part)!=std::string::npos,e.what());return;}
    throw std::runtime_error("expected contract rejection");
}
SimulationConfiguration config() {
    SimulationConfiguration c;c.device.geometry.host_channels=1;c.device.geometry.blocks_per_bank=32;
    c.host.max_resident_fragments=128;return c;
}
GlobalRequest wr(std::uint64_t id,std::uint64_t addr=0,Tick at={},std::uint64_t content=1) {
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{addr},4096,content};r.not_before=at;return r;
}
GlobalRequest rd(std::uint64_t id,Tick at={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Read,GlobalByteAddress{},64};r.not_before=at;r.expected_content=1;return r;
}
void validate(SimulationSession& s) {HostIndexTestAccess::validate(s.host());}
void reclaimed(SimulationSession& s) {
    validate(s);const auto n=s.host().storage();
    check(!n.work&&!n.units&&!n.requests&&!n.attempts&&!s.input_capacity().resident,"safe detail and capacity reclamation");
}
SessionReport finish(SimulationSession& s,bool success=true) {
    s.close_input();auto r=s.run_to_completion();
    check(r.run.status==RunStatus::Completed,r.run.explanation.c_str());
    check(r.run.workload_success==success,"logical outcome");
    check(r.run.host.drained()&&r.run.device.drained(),"both owners drain");reclaimed(s);return r;
}
void idle(SimulationSession& s) {
    auto r=s.advance_before(Tick{s.now().value+10000000});
    check(r.run.status==RunStatus::WaitingForInput,r.run.explanation.c_str());reclaimed(s);
}
auto success=[](const ProgramOperation&){return ProgramOutcome::Success;};
void install_guards() {
    SimulationSession empty(resolve_configuration(config()));
    rejects([&]{empty.set_program_outcome({});},"nonempty");empty.set_program_outcome(success);finish(empty);
    for(int boundary=0;boundary<3;++boundary) {
        SimulationSession s(resolve_configuration(config()));s.set_program_outcome(success);
        if(boundary==0)s.submit(wr(1));
        if(boundary==1)s.advance_before(Tick{10});
        if(boundary==2)s.close_input();
        rejects([&]{s.set_program_outcome(success);},"before session");finish(s);
    }
}
void install_atomic() {
    auto c=config();c.host.max_resident_fragments=64;SimulationSession s(resolve_configuration(c));
    auto invalid=wr(1);invalid.bytes=63;
    rejects([&]{s.submit(invalid);},"unaligned");
    auto large=wr(1);large.bytes=8192;
    check(s.try_submit(large).disposition==InputDisposition::TooLarge,"oversized input is not registration");
    unsigned calls=0;s.set_program_outcome([&](const auto&){++calls;return ProgramOutcome::Success;});
    s.submit(wr(1));finish(s);check(calls==1,"atomic rejection leaves setup open");
}
void install_reentrant() {
    SimulationSession s(resolve_configuration(config()));unsigned program_calls=0,completions=0;
    s.set_program_outcome([&](const auto&){++program_calls;rejects([&]{s.set_program_outcome(success);},"external-only");return ProgramOutcome::Success;});
    s.set_completion_handler([&](const auto&){++completions;rejects([&]{s.set_program_outcome(success);},"external-only");});
    s.submit(wr(1));finish(s);check(program_calls==1&&completions==1,"rejected mutation preserves callback and final result");
}
void fault_override() {
    auto c=config();c.faults.fail_sequence={0};SimulationSession s(resolve_configuration(c));
    unsigned discarded=0,calls=0;
    s.set_program_outcome([&](const auto&){++discarded;return ProgramOutcome::FailedReusable;});
    s.set_program_outcome([&](const auto&){++calls;return ProgramOutcome::Success;});
    rejects([&]{s.set_program_outcome({});},"nonempty");
    s.submit(wr(1));finish(s);
    check(calls==1&&!discarded&&!s.host().statistics().recovery_tasks,"callback replaces configured and previous models");
}
void fault_exception() {
    SimulationSession s(resolve_configuration(config()));
    s.set_program_outcome([](const auto&)->ProgramOutcome{throw std::runtime_error("fault callback failed");});
    s.submit(wr(1));s.close_input();auto r=s.run_to_completion();
    check(r.run.status==RunStatus::ImplementationError&&r.run.explanation.find("fault callback failed")!=std::string::npos,"callback failure is terminal, not a program-fail response");
    rejects([&]{s.set_program_outcome(success);},"terminal");
    check(s.run_to_completion().run.events==r.run.events,"terminal report does not execute more events");
}
void history(bool fail) {
    SimulationSession s(resolve_configuration(config()));unsigned calls=0,completions=0;
    s.set_program_outcome([&](const auto&){const auto index=calls++;return fail&&index==16?ProgramOutcome::FailedReusable:ProgramOutcome::Success;});
    s.set_completion_handler([&](const auto&){++completions;validate(s);});
    // Keep many histories while collecting all detailed normal Units.
    for(unsigned i=0;i<16;++i) {s.submit(wr(i+1,i*32768,s.now()));idle(s);}
    check(!HostIndexTestAccess::write_error_seen(s.host())&&s.host().storage().versions==16,"healthy history does not activate error processing");
    auto read=rd(17,s.now());read.bytes=4096;s.submit(read);idle(s);
    s.submit(wr(18,4096,s.now()));finish(s);
    check(completions==18&&s.host().results().size()==18,"one result per original Global");
    check(HostIndexTestAccess::write_error_seen(s.host())==fail,"error history gate is sticky after recovery");
    check(s.host().statistics().replay_commands==(fail?128u:0u),"late failure restores collected prefix without losing retention");
}
void history_normal(){history(false);}
void history_late_failure(){history(true);}
void persistent_failure() {
    auto c=config();c.host.max_replay_passes=1;SimulationSession s(resolve_configuration(c));unsigned calls=0;
    s.set_program_outcome([&](const auto&){++calls;return ProgramOutcome::FailedReusable;});
    s.submit(wr(1));finish(s,false);
    check(calls==2&&s.host().statistics().replay_passes==1,"persistent errors obey original recovery budget");
    check(s.host().results().at(GlobalRequestId{1}).reason=="RecoveryBudgetExhausted","failure reason preserved");
}
void read_failure_dependency() {
    SimulationSession s(resolve_configuration(config()));s.submit(wr(1));idle(s);
    auto bad=rd(2,s.now());bad.expected_content=999;
    auto dependent=rd(3,Tick{s.now().value+100000000});dependent.after={GlobalRequestId{2}};
    s.submit(std::vector<GlobalRequest>{bad,dependent});auto r=finish(s,false);
    check(!HostIndexTestAccess::write_error_seen(s.host()),"read verification failure is not a write error");
    check(s.host().results().at(GlobalRequestId{3}).reason=="DependencyFailed"&&r.run.at<dependent.not_before,"read failure cancellation and obsolete-event cleanup remain active");
}
void abort_later_version() {
    auto c=config();c.host.max_replay_passes=0;SimulationSession s(resolve_configuration(c));
    s.set_program_outcome([](const auto&){return ProgramOutcome::FailedReusable;});
    s.submit(wr(1));idle(s);check(HostIndexTestAccess::write_error_seen(s.host()),"failed history remains after collection");
    s.submit(wr(2,0,s.now(),2));finish(s,false);
    check(s.host().results().at(GlobalRequestId{2}).reason=="PriorContentFailed","future content still observes old failure");
}
void zero_reclaim() {
    SimulationSession s(resolve_configuration(config()));s.submit(wr(1,0,Tick{1000000}));
    s.advance_before(Tick{100});validate(s);const auto n=s.host().storage();
    check(n.units==64&&s.input_capacity().resident==64,"future normal Units are not reclaimed");
    s.advance_before(Tick{100});validate(s);finish(s);
}
}
int main(int argc,char** argv) {
    const std::map<std::string,void(*)()> tests={{"install_guards",install_guards},{"install_atomic",install_atomic},
        {"install_reentrant",install_reentrant},{"fault_override",fault_override},{"fault_exception",fault_exception},
        {"history_normal",history_normal},{"history_late_failure",history_late_failure},{"persistent_failure",persistent_failure},
        {"read_failure_dependency",read_failure_dependency},{"abort_later_version",abort_later_version},{"zero_reclaim",zero_reclaim}};
    try {if(argc!=2)throw std::runtime_error("scenario required");tests.at(argv[1])();std::cout<<"PASS backend overlay "<<argv[1]<<'\n';}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
