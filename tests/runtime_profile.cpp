#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "hbf_eventlab/statistics/event_stream.hpp"
#include <iostream>
#include <sstream>
#include <map>
using namespace hbf_eventlab;
namespace {
void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("missing rejection");}
std::uint64_t time_ns=0,reads=0;
std::uint64_t clock_ns() noexcept {++reads;return time_ns;}
void nested(){
    RuntimeProfile p(clock_ns);time_ns=0;
    {RuntimeScope outer(&p,RuntimeRegion::Prepare);time_ns=10;
        {RuntimeScope inner(&p,RuntimeRegion::Split);time_ns=30;}
        time_ns=50;
    }
    const auto a=p.measurement(RuntimeRegion::Prepare),b=p.measurement(RuntimeRegion::Split);
    check(a.calls==1&&a.inclusive_ns==50&&a.self_ns==30&&a.max_ns==50,"parent excludes child");
    check(b.inclusive_ns==20&&b.self_ns==20&&p.covered_root_ns()==50,"nonoverlapping root time");
    time_ns=60;{RuntimeScope scope(&p,RuntimeRegion::Split);time_ns=70;}
    check(p.measurement(RuntimeRegion::Split).calls==2&&p.measurement(RuntimeRegion::Split).max_ns==20&&p.covered_root_ns()==60,"aggregation/max");
}
void disabled(){
    reads=0;RuntimeProfile unused(clock_ns);
    {RuntimeScope scope(nullptr,RuntimeRegion::Split);}
    check(reads==0&&unused.covered_root_ns()==0,"disabled path never reads clock");
}
void unwind(){
    time_ns=0;RuntimeProfile p(clock_ns);
    rejects([&]{RuntimeScope a(&p,RuntimeRegion::Prepare);RuntimeScope b(&p,RuntimeRegion::Split);time_ns=10;throw std::runtime_error("test");});
    check(p.measurement(RuntimeRegion::Prepare).unwound_calls==1&&p.measurement(RuntimeRegion::Split).unwound_calls==1,"exceptions accounted");
    {RuntimeScope a(&p,RuntimeRegion::Prepare);time_ns=20;}
    check(p.covered_root_ns()==20&&p.measurement(RuntimeRegion::Prepare).calls==2,"stack restored after exception");
}
void export_contract(){
    RuntimeProfile p(clock_ns);std::ostringstream out;
    {RuntimeScope a(&p,RuntimeRegion::Split);rejects([&]{p.write_json(out);});}
    p.add(RuntimeCounter::SplitFragments,64);p.peak(RuntimeCounter::ResultsSizePeak,3);p.peak(RuntimeCounter::ResultsSizePeak,2);
    p.write_json(out);check(out.str().find("\"split_fragments\":64")!=std::string::npos&&p.counter(RuntimeCounter::ResultsSizePeak)==3,"export counters");
    check(out.str().find("program_wall_ns")!=std::string::npos,"wall-time units explicit");
    check(out.str().find("\"clock\":\"custom\"")!=std::string::npos,"injected test clock is labelled accurately");
    std::ostringstream bad;bad.setstate(std::ios::badbit);rejects([&]{p.write_json(bad);});
}
void isolation(){
    RuntimeProfile a(clock_ns),b(clock_ns);time_ns=0;
    {RuntimeScope x(&a,RuntimeRegion::Prepare);time_ns=10;{RuntimeScope y(&b,RuntimeRegion::Split);time_ns=20;}time_ns=30;}
    check(a.measurement(RuntimeRegion::Prepare).self_ns==30&&b.covered_root_ns()==10,"independent stacks");
}
SimulationConfiguration config(){SimulationConfiguration c;c.device.geometry.host_channels=1;c.host.max_resident_fragments=64;return c;}
GlobalRequest wr(std::uint64_t id,std::uint64_t bytes=4096){return {GlobalRequestId{id},Op::Write,GlobalByteAddress{},bytes,1};}
GlobalRequest rd(std::uint64_t id){return {GlobalRequestId{id},Op::Read,GlobalByteAddress{},64};}
void admission(){
    RuntimeProfile p;SimulationSession s(resolve_configuration(config()),nullptr,&p);
    check(s.try_submit(wr(1,8192)).disposition==InputDisposition::TooLarge,"oversize rejected");
    check(p.counter(RuntimeCounter::SplitFragments)==0,"TooLarge never materializes");
    s.submit(wr(1));
    check(s.try_submit(rd(2)).disposition==InputDisposition::Backpressured,"capacity backpressure");
    check(p.counter(RuntimeCounter::SplitFragments)==65&&p.counter(RuntimeCounter::RegisteredFragments)==64,"prepared rejected fragment counted separately");
    check(p.counter(RuntimeCounter::InputTooLarge)==1&&p.counter(RuntimeCounter::InputBackpressured)==1&&p.counter(RuntimeCounter::BackpressuredFragments)==1,"admission outcomes");
    rejects([&]{s.submit(wr(1));});
    check(p.measurement(RuntimeRegion::Prepare).unwound_calls==1&&p.counter(RuntimeCounter::RegisteredGlobals)==1,"invalid input has no registration");
    s.close_input();const auto r=s.run_to_completion();check(r.run.workload_success,"successful drain");
    check(p.counter(RuntimeCounter::ReclaimedWork)==64&&p.counter(RuntimeCounter::ReclaimedUnits)==64&&p.counter(RuntimeCounter::ReclaimedRequests)==1,"reclamation counters");
    check(p.counter(RuntimeCounter::ResultsEntriesVisited)==0&&p.measurement(RuntimeRegion::ResultsScan).calls==0,"no history scan charged");
    check(p.measurement(RuntimeRegion::CompletionCheck).calls>0&&
          p.measurement(RuntimeRegion::CompletionCheck).calls==p.measurement(RuntimeRegion::Advance).calls,"one aggregate check per advance");
    check(p.counter(RuntimeCounter::HandoffTaken)==s.host().statistics().handed,"handoff accounting");
}
struct Run {std::string events;Tick at;std::uint64_t executed,responses,replays,timeouts,pending_retries;bool success;};
Run execute(RuntimeProfile* p,int mode){
    auto c=config();if(mode==1)c.faults.fail_sequence={0};
    if(mode==2){c.host.max_timeout_retries=1;c.device.selected.mocs_selected=1;c.device.selected.assembly_timeout=Tick{50000};}
    if(mode==3){c.host.max_resident_fragments=128;c.device.capabilities.max_pending_write_dlus=1;
        c.host.max_open_dlus_per_channel=3;c.host.pending_retry_delay=Tick{10000000};c.execution.media_timing.program=Tick{3000000};}
    std::ostringstream data;StatisticsEventStream stream(data,256);SimulationSession s(resolve_configuration(c),&stream,p);
    s.submit(wr(1));if(mode==3){auto r=wr(2);r.address=GlobalByteAddress{32768};s.submit(r);}
    s.close_input();auto r=s.run_to_completion();
    check(r.run.status==RunStatus::Completed&&r.run.host.drained()&&r.run.device.drained(),"all paths terminate/drain");
    const auto& h=s.host().statistics();stream.finish(r.run.at,to_string(r.run.status));
    return {data.str(),r.run.at,r.run.events,h.responses,h.replay_passes,h.timeout_retry_rounds,h.pending_retry_commands,r.run.workload_success};
}
void observational(int mode){
    const auto off=execute(nullptr,mode);RuntimeProfile p;const auto on=execute(&p,mode);
    check(off.events==on.events&&off.at==on.at&&off.executed==on.executed&&off.success==on.success,"identical semantic events and time");
    check(on.responses==p.measurement(RuntimeRegion::Receive).calls,"receive covers command lifecycle");
    if(mode==1)check(on.replays>0&&on.success,"replay exercised");
    if(mode==2)check(on.timeouts>0&&!on.success,"timeout/retry exercised");
    if(mode==3)check(on.pending_retries>0&&on.success,"Pending-full retry exercised");
}
void config_entry(){
    auto c=config();check(c.output.runtime_profile_json.empty(),"off by default");
    apply_configuration_override(c,"output.runtime_profile_json","profile.json");
    auto resolved=resolve_configuration(c);std::ostringstream out;write_effective_configuration(out,resolved);
    std::istringstream in(out.str());check(read_configuration(in).output.runtime_profile_json=="profile.json","effective configuration round trip");
}
}
int main(int argc,char**argv){try{
    const std::map<std::string,void(*)()> cases={{"nested",nested},{"disabled",disabled},{"unwind",unwind},{"export",export_contract},
        {"isolation",isolation},{"admission",admission},{"normal",[]{observational(0);}},{"replay",[]{observational(1);}},
        {"timeout",[]{observational(2);}},{"pending",[]{observational(3);}},{"config",config_entry}};
    if(argc!=2)throw std::runtime_error("scenario required");cases.at(argv[1])();std::cout<<"PASS runtime_profile "<<argv[1]<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
