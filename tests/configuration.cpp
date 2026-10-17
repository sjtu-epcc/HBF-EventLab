#include "hbf_eventlab/configuration/simulation_configuration.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
template<class F> void rejects(F f,const std::string& text="") {
    bool bad=false;try{f();}catch(const std::exception& e){bad=text.empty()||std::string(e.what()).find(text)!=std::string::npos;}
    check(bad,"expected configuration diagnostic");
}
SimulationConfiguration parse(const std::string& text) {std::istringstream in(text);return read_configuration(in,"test.ini");}
GlobalRequest write(std::uint64_t bytes=4096){return {GlobalRequestId{1},Op::Write,GlobalByteAddress{},bytes,1};}
GlobalRequest read(){return {GlobalRequestId{1},Op::Read,GlobalByteAddress{},64};}
struct Result {RunReport report;HostStatistics host;DeviceStatistics device;};
Result run(SimulationConfiguration c,const std::vector<GlobalRequest>& input) {
    const auto resolved=resolve_configuration(std::move(c));const auto& v=resolved.values();
    EventQueue events;DeviceDriver device(resolved.description(),v.execution,events);
    RoundRobinInterleaver inter(v.address.global_base,v.device.geometry.host_channels,v.device.geometry.channel_bytes(),v.address.interleave_bytes);
    HostDriver host(device,events,inter,v.host);device.set_program_outcome(make_program_outcome(v.faults));
    host.load(input);host.start();SimulationRunner runner(events,host,device);
    return {runner.run(v.run),host.statistics(),device.statistics()};
}
SimulationConfiguration one_channel() {SimulationConfiguration c;c.device.geometry.host_channels=1;return c;}
void defaults(){
    const auto r=resolve_configuration({});const auto& c=r.values();
    check(c.device.selected.zone_size_dlus==8&&c.device.topology.die_to_core_channel==std::vector<std::uint32_t>{0},"automatic values resolved");
    check(c.execution.ucie.effective_gbytes_per_second()==48&&c.device.capabilities.mocs_supported==256,"baseline retained");
    check(r.description().geometry().channel_bytes()==131072,"derived channel capacity");
}
void parse_override(){
    auto c=parse("# comment\n[device.capabilities]\nmocs_supported=1024\n[device.selected]\nmocs_selected=512\n[host]\nmax_open_dlus_per_channel=7\n");
    apply_configuration_override(c,"device.selected.mocs_selected","0x100");
    const auto r=resolve_configuration(c);
    check(r.description().configuration().mocs_selected==256&&r.values().host.max_open_dlus_per_channel==7,"explicit override preserves other file values");
}
void parser_errors(){
    for(const auto& text:{"[host\n","[unknown]\n","[host]\nonsense=2\n","host.max_open_dlus_per_channel=2\n[host]\nmax_open_dlus_per_channel=3\n","[host]\nwrite_mode=\"unterminated\n","[host]\nwrite_mode=\"fragment64\" garbage\n","missing equals\n"})
        rejects([&]{parse(text);},"test.ini:");
}
void unknown_fields(){
    auto c=one_channel();
    for(const auto& key:{"execution.ucie.effective_gbytes_per_second","device.max_stack_height","host.typod_parameter","execution.read_dispatch.unused"})
        rejects([&]{apply_configuration_override(c,key,"48");},"unknown or derived-only");
}
void numeric(){
    auto c=one_channel();
    for(const auto& s:{"-1","+1","1.0","12oops","18446744073709551616",""})
        rejects([&]{apply_configuration_override(c,"address.interleave_bytes",s);},"address.interleave_bytes");
    rejects([&]{apply_configuration_override(c,"device.geometry.host_channels","4294967296");},"field range");
    rejects([&]{apply_configuration_override(c,"device.capabilities.write_burst","1");},"true or false");
    rejects([&]{apply_configuration_override(c,"device.topology.die_to_core_channel","[0,]");},"empty list");
    rejects([&]{apply_configuration_override(c,"device.topology.die_to_core_channel","[0,,1]");});
}
void grades(){
    for(unsigned n=1;n<=3;++n){
        auto c=one_channel();c.execution.ucie.speed_grade=static_cast<UcieSpeedGrade>(n);
        const auto& p=ucie_speed_profile(c.execution.ucie.speed_grade);
        c.device.geometry.host_channels=p.max_modules;c.device.geometry.dies_per_channel=p.max_stack_height;
        const auto r=resolve_configuration(c);
        const std::uint64_t expected[]={48,94,188};check(r.values().execution.ucie.effective_gbytes_per_second()==expected[n-1],"grade-derived rates");
        check(r.values().execution.ucie.serialization(64)==Tick{64000/expected[n-1]+(64000%expected[n-1]!=0)},"ceil service time");
    }
}
void grade_limits(){
    auto c=one_channel();c.device.geometry.host_channels=9;rejects([&]{resolve_configuration(c);},"module limit");
    c.device.geometry.host_channels=1;c.device.geometry.dies_per_channel=16;rejects([&]{resolve_configuration(c);},"stack limit");
    c.execution.ucie.speed_grade=UcieSpeedGrade::Grade2;(void)resolve_configuration(c);
    c.device.geometry.dies_per_channel=2;check(resolve_configuration(c).values().device.geometry.dies_per_channel==2,"grade does not force maximum population");
    c.execution.ucie.speed_grade=static_cast<UcieSpeedGrade>(4);rejects([&]{resolve_configuration(c);},"ucie_speed_grade");
}
void mocs_ranges(){
    for(auto n:{1u,256u,512u,1024u,16384u}){auto c=one_channel();c.device.capabilities.mocs_supported=n;c.device.selected.mocs_selected=n;(void)resolve_configuration(c);}
    for(auto n:{0u,2u,64u,255u,300u,32768u}){auto c=one_channel();c.device.selected.mocs_selected=n;rejects([&]{resolve_configuration(c);},"mocs");}
    auto c=one_channel();c.device.selected.mocs_selected=512;rejects([&]{resolve_configuration(c);},"mocs");
    c.device.capabilities.mocs_supported=1024;check(resolve_configuration(c).description().configuration().mocs_selected==512,"supported independent of selected");
}
void small_read(){
    auto c=one_channel();c.device.capabilities.write_burst=false;c.device.selected.mocs_selected=1;
    check(diagnose_workload(resolve_configuration(c),{read()}).empty(),"pure read has no write progress warning");
    const auto r=run(c,{read()});check(r.report.status==RunStatus::Completed&&r.host.responses==1,"single-credit read drains even without burst capability");
    check(r.device.senses==1&&r.report.device.drained(),"unwritten read executes and returns its normal status");
}
void small_write(){
    for(bool burst_cap:{false,true}){
        auto c=one_channel();c.device.capabilities.write_burst=burst_cap;c.device.selected.mocs_selected=1;c.host.max_timeout_retries=0;
        check(diagnose_workload(resolve_configuration(c),{write()}).size()==1,"actual mode determines warning, not capability");
        const auto r=run(c,{write()});check(r.report.status==RunStatus::Completed&&!r.report.workload_success,"legal overload reaches logical failure");
        check(r.host.assembly_timeout_errors>0&&r.device.programs==0&&r.report.device.drained(),"actual timeout, not input rejection or fake progress");
    }
}
void small_burst(){
    auto c=one_channel();c.device.selected.mocs_selected=1;c.host.writes=WriteCommandMode::Burst4096;
    check(diagnose_workload(resolve_configuration(c),{write()}).empty(),"one full burst needs one credit");
    const auto r=run(c,{write()});check(r.report.workload_success&&r.host.responses==1&&r.device.programs==1,"single-credit burst succeeds");
}
void capabilities(){
    auto c=one_channel();c.device.capabilities.read_burst=true;rejects([&]{resolve_configuration(c);},"read_burst");
    c.device.capabilities.read_burst=false;c.device.capabilities.write_burst=false;c.host.writes=WriteCommandMode::Burst4096;
    rejects([&]{resolve_configuration(c);},"requires");
}
void geometry(){
    auto c=one_channel();c.device.geometry.blocks_per_bank=0;rejects([&]{resolve_configuration(c);});
    c=one_channel();c.device.geometry.dies_per_channel=3;rejects([&]{resolve_configuration(c);});
    c=one_channel();c.device.geometry.blocks_per_bank=UINT64_MAX;rejects([&]{resolve_configuration(c);},"overflow");
    c=one_channel();c.device.selected.zone_size_dlus=3;rejects([&]{resolve_configuration(c);},"zone_size_dlus");
    rejects([&]{apply_configuration_override(c,"device.selected.zone_size_dlus","0");},"auto");
}
void topology(){
    auto c=one_channel();c.device.geometry.dies_per_channel=4;c.device.topology.core_channels_per_ucie=2;
    auto r=resolve_configuration(c);check(r.description().topology().die_to_core_channel==std::vector<std::uint32_t>({0,1,0,1}),"auto routing");
    c.device.topology.die_to_core_channel={1,1,0,0};r=resolve_configuration(c);
    EventQueue events;DeviceDriver d(r.description(),r.values().execution,events);
    check(d.topology().core_channel(0)==1&&d.topology().core_channel(3)==0,"configured route reaches actual device");
    c.device.topology.die_to_core_channel={0};rejects([&]{resolve_configuration(c);},"one entry per die");
    c.device.topology.die_to_core_channel={0,1,2,0};rejects([&]{resolve_configuration(c);},"index");
}
void capacity(){
    auto c=one_channel();c.device.geometry.blocks_per_bank=20;c.device.capacity_policy.max_hole_permille=100;
    const auto r=resolve_configuration(c);EventQueue events;DeviceDriver d(r.description(),r.values().execution,events);
    check(d.capacity_hole_limit(0)==2,"capacity policy reaches live coordinator");
    c.device.capacity_policy.max_hole_permille=1001;rejects([&]{resolve_configuration(c);},"permille");
}
void buffers(){
    auto c=one_channel();c.execution.buffers.ingress.payload_bytes=64;rejects([&]{resolve_configuration(c);},"largest advertised");
    c.device.capabilities.write_burst=false;(void)resolve_configuration(c);
    c.execution.buffers.egress.payload_bytes=63;rejects([&]{resolve_configuration(c);},"64B");
    c.execution.buffers.egress.payload_bytes=64;c.execution.buffers.ingress.entries=0;rejects([&]{resolve_configuration(c);},"ingress_entries");
}
void timing(){
    auto c=one_channel();c.host.writes=WriteCommandMode::Burst4096;c.execution.media_timing.program=Tick{100};
    const auto first=run(c,{write()});c.execution.media_timing.program=Tick{200};const auto second=run(c,{write()});
    check(second.report.at.value-first.report.at.value==100,"timing config changes actual completion time");
    c.execution.media_timing.program=Tick{};rejects([&]{resolve_configuration(c);},"program_ps");
    c.execution.media_timing.program=Tick{1};c.execution.ucie.overhead.request_control_bytes=0;rejects([&]{resolve_configuration(c);},"request_control_bytes");
    c.execution.ucie.overhead.request_control_bytes=UINT64_MAX;rejects([&]{resolve_configuration(c);},"overflow");
    c.execution.ucie.overhead.request_control_bytes=16;c.execution.ucie.overhead.request_fixed_latency=Tick{UINT64_MAX};
    rejects([&]{resolve_configuration(c);},"overflow");
}
void zeros(){
    auto c=one_channel();c.execution.read_dispatch.wait=Tick{};c.execution.read_dispatch.threshold=0;
    c.execution.ucie.overhead.request_fixed_latency=Tick{};c.execution.ucie.overhead.response_fixed_latency=Tick{};
    c.host.max_pending_retries=0;c.host.max_timeout_retries=0;c.host.max_replay_passes=0;c.host.pending_retry_delay=Tick{};c.host.retained_fragments=0;
    c.run.events={0,0,0};c.run.max_tick.reset();
    const auto r=run(c,{read()});check(r.report.status==RunStatus::Completed,"valid zero meanings retained");
    c.run.max_tick=Tick{};check(run(c,{read()}).report.status==RunStatus::LimitReached,"zero max Tick is not unlimited");
    c.host.max_pending_retries=1;rejects([&]{resolve_configuration(c);},"pending_delay_ps");
}
void policies(){
    for(const auto& key:{"host.send_policy","execution.read_dispatch.policy","execution.nand_scheduler.policy","execution.ucie_scheduler.policy"}){
        auto c=one_channel();apply_configuration_override(c,key,"not_implemented");rejects([&]{resolve_configuration(c);},key);
    }
    auto c=one_channel();c.execution.read_dispatch.policy="custom";rejects([&]{resolve_configuration(c);},"unsupported policy");
}
void custom_policy(){
    auto c=resolve_configuration(one_channel());EventQueue events;
    DeviceDriver d(c.description(),c.values().execution,events,std::make_unique<FcfsNandPolicy>(),std::make_unique<FifoUciePolicy>(),std::make_unique<WindowDispatchPolicy>(Tick{7}));
    check(d.configuration().nand_scheduler=="custom"&&d.configuration().ucie_scheduler=="custom"&&d.configuration().read_dispatch.policy=="custom","injected implementations not mislabeled as configured defaults");
}
void address(){
    auto c=one_channel();c.address.global_base=GlobalByteAddress{4096};c.address.interleave_bytes=128;
    auto w=write();w.address=GlobalByteAddress{4096};check(run(c,{w}).report.workload_success,"global base removed once");
    c.address.global_base=GlobalByteAddress{1};rejects([&]{resolve_configuration(c);},"address");
    c.address.global_base={};c.address.interleave_bytes=0;rejects([&]{resolve_configuration(c);},"address");
    c.address.interleave_bytes=UINT64_MAX;rejects([&]{resolve_configuration(c);},"address");
}
void faults(){
    auto c=one_channel();c.faults.target.block=1;c.faults.fail_sequence={2,1};const auto r=resolve_configuration(c);
    auto f=make_program_outcome(r.values().faults);ProgramOperation op{};op.physical.page=2;
    check(f(op)==ProgramOutcome::Success,"other block does not consume fault sequence");
    op.logical.block=1;check(f(op)==ProgramOutcome::FailedReusable,"targeted root fault");op.physical.page=1;
    check(f(op)==ProgramOutcome::FailedReusable&&f(op)==ProgramOutcome::Success,"ordered one-shot sequence");
    c.faults.persistent_page=0;rejects([&]{resolve_configuration(c);},"OR");
    c.faults.fail_sequence={};c.faults.persistent_page=8;rejects([&]{resolve_configuration(c);},"outside geometry");
}
void roundtrip(){
    auto c=parse("\xEF\xBB\xBF[device]\nucie_speed_grade=2\n[device.geometry]\ndies_per_channel=4\n[host.retry]\nmax_pending_retries=7\n[output]\nstatistics_json=\"some folder/result.json\"\n[run]\nmax_tick_ps=none\n");
    std::ostringstream a;write_effective_configuration(a,resolve_configuration(c));
    std::ostringstream b;write_effective_configuration(b,resolve_configuration(parse(a.str())));
    check(a.str()==b.str(),"effective configuration roundtrip includes all fields and resolved values");
    check(a.str().find("effective_GBps_per_direction_per_module = 94")!=std::string::npos,"derived grade published");
}
void frozen(){
    auto c=one_channel();auto r=resolve_configuration(c);c.device.capabilities.max_pending_write_dlus=1;
    check(r.description().capabilities().max_pending_write_dlus==8,"resolved copy isolated from input mutations");
    check(r.values().device.selected.zone_size_dlus==8,"resolved geometry constants available");
}
void all_keys(){
    const auto c=resolve_configuration({});std::ostringstream exported;write_effective_configuration(exported,c);
    const auto p=parse(exported.str());
    std::ostringstream summary;write_configuration_summary(summary,resolve_configuration(p));
    check(summary.str().find("mocs_supported=256")!=std::string::npos&&summary.str().find("pending_limit=8")!=std::string::npos,"summary from actual values");
}
void default_equivalence(){
    auto a=one_channel();const auto r1=run(a,{write()});
    std::ostringstream out;write_effective_configuration(out,resolve_configuration(a));const auto r2=run(parse(out.str()),{write()});
    check(r1.report.at==r2.report.at&&r1.report.events==r2.report.events&&r1.host.responses==r2.host.responses,"configuration loading is observational");
}
}
int main(int argc,char**argv){
    const std::map<std::string,void(*)()> tests={{"defaults",defaults},{"parse_override",parse_override},{"parser_errors",parser_errors},
        {"unknown_fields",unknown_fields},{"numeric",numeric},{"grades",grades},{"grade_limits",grade_limits},{"mocs_ranges",mocs_ranges},
        {"small_read",small_read},{"small_write",small_write},{"small_burst",small_burst},{"capabilities",capabilities},{"geometry",geometry},
        {"topology",topology},{"capacity",capacity},{"buffers",buffers},{"timing",timing},{"zeros",zeros},{"policies",policies},
        {"custom_policy",custom_policy},{"address",address},{"faults",faults},{"roundtrip",roundtrip},{"frozen",frozen},
        {"all_keys",all_keys},{"default_equivalence",default_equivalence}};
    try{if(argc!=2||!tests.count(argv[1]))throw std::invalid_argument("unknown scenario");tests.at(argv[1])();std::cout<<"PASS configuration "<<argv[1]<<" checks="<<checks<<"\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL "<<(argc>1?argv[1]:"")<<": "<<e.what()<<"\n";return 1;}
}
