#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/adapters/workload_reader.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace {
std::uint64_t number(const std::string&s){
    if(s.empty()||s[0]=='-'||s[0]=='+')throw std::invalid_argument("expected unsigned number");
    std::size_t end=0;auto n=std::stoull(s,&end,s.rfind("0x",0)==0||s.rfind("0X",0)==0?16:10);
    if(end!=s.size())throw std::invalid_argument("invalid number");return n;
}
unsigned small(const std::string&s){auto n=number(s);if(n>UINT32_MAX)throw std::invalid_argument("integer too large");return static_cast<unsigned>(n);}
const char* result(LogicalResult r){return r==LogicalResult::Success?"Success":r==LogicalResult::Failed?"Failed":"Pending";}
}
int main(int argc,char**argv){
    try{
        if(argc<2){std::cerr<<"usage: hbf_eventlab_cli workload.txt [--channels N] [--interleave N] [--base N] [--burst]\n"
            <<"  [--regular] [--open N] [--replays N] [--retention N] [--mocs N]\n  [--pending-retries N] [--timeout-retries N] [--retry-delay N]\n"
            <<"  [--fail-sequence page,page] [--persistent-page N] [--max-tick N] [--max-events N] [--trace]\n";return 5;}
        HostConfiguration hc;ExecutionConfiguration ec;RunLimits limits;unsigned channels=2,mocs=256;
        std::uint64_t base=0,interleave=64;std::vector<PageId> faults;std::optional<PageId> persistent;bool trace=false;
        for(int i=2;i<argc;++i){
            std::string key=argv[i];
            if(key=="--burst"){hc.writes=WriteCommandMode::Burst4096;continue;}
            if(key=="--regular"){hc.hints=ReadHintMode::AllRegular;continue;}
            if(key=="--trace"){trace=true;continue;}
            if(i+1>=argc)throw std::invalid_argument("missing option value: "+key);
            std::string value=argv[++i];
            if(key=="--channels")channels=small(value);
            else if(key=="--interleave")interleave=number(value);
            else if(key=="--base")base=number(value);
            else if(key=="--open")hc.max_open_dlus_per_channel=number(value);
            else if(key=="--replays")hc.max_replay_passes=small(value);
            else if(key=="--pending-retries")hc.max_pending_retries=small(value);
            else if(key=="--timeout-retries")hc.max_timeout_retries=small(value);
            else if(key=="--retry-delay")hc.pending_retry_delay=Tick{number(value)};
            else if(key=="--retention")hc.retained_fragments=number(value);
            else if(key=="--mocs")mocs=small(value);
            else if(key=="--max-tick")limits.max_tick=Tick{number(value)};
            else if(key=="--max-events")limits.events.total_events=number(value);
            else if(key=="--persistent-page")persistent=small(value);
            else if(key=="--fail-sequence"){
                if(value.empty()||value.back()==',')throw std::invalid_argument("empty fault page");
                std::istringstream in(value);std::string page;while(std::getline(in,page,','))faults.push_back(small(page));
            }else throw std::invalid_argument("unknown option: "+key);
        }
        Geometry geometry{channels,1,1,8,4,1};
        for(auto page:faults)if(page>=geometry.pages_per_block)throw std::invalid_argument("fault page outside geometry");
        if(persistent&&*persistent>=geometry.pages_per_block)throw std::invalid_argument("fault page outside geometry");
        StaticDeviceDescription desc{geometry,{256,8,false,true},{mocs,Tick{10000000},geometry.block_group_dlus()}};
        EventQueue events;DeviceDriver device(desc,ec,events);
        RoundRobinInterleaver inter(GlobalByteAddress{base},channels,geometry.channel_bytes(),interleave);
        HostDriver host(device,events,inter,hc);
        // Explicit experiment input. Host sees only command responses, never this injector.
        device.set_program_outcome([faults,persistent,index=std::size_t{}](const ProgramOperation& op)mutable{
            if(op.logical.channel!=0||op.logical.die!=0||op.logical.bank!=0||op.logical.block!=0)return ProgramOutcome::Success;
            if(persistent&&op.physical.page==*persistent)return ProgramOutcome::FailedReusable;
            if(index<faults.size()&&op.physical.page==faults[index]){++index;return ProgramOutcome::FailedReusable;}
            return ProgramOutcome::Success;
        });
        if(trace){
            host.set_trace([](auto&e){std::cout<<"HOST "<<e.at.value<<" "<<e.action<<" cmd="<<e.command.value
                <<" unit="<<e.unit.value<<" channel="<<e.block.channel<<" block="<<e.block.block<<" pass="<<e.pass<<"\n";});
            device.set_trace([](auto&e){std::cout<<"DEVICE "<<e.at.value<<" "<<e.action<<" channel="<<e.channel<<" id="<<e.identity<<"\n";});
        }
        std::ifstream input(argv[1]);if(!input)throw std::invalid_argument("cannot open workload file");
        host.load(read_workload(input));host.start();SimulationRunner runner(events,host,device);const auto report=runner.run(limits);
        std::cout<<"HBF EventLab | functional timing, NOT calibrated performance\n"
            <<"CONFIG channels="<<channels<<" dies/channel=1 banks/die=1 pages/block=8 blocks/bank=4 base="<<base
            <<" interleave_bytes="<<interleave<<" command_write_bytes="<<(hc.writes==WriteCommandMode::Fragment64?64:4096)
            <<" hints="<<(hc.hints==ReadHintMode::TailBatch?"tail-batch":"all-regular")
            <<" open_dlus="<<hc.max_open_dlus_per_channel<<" mocs="<<mocs<<" pending_limit=8\n"
            <<"CONFIG tick=1ps sense="<<ec.sense.value<<" program="<<ec.program.value<<" erase="<<ec.erase.value
            <<" cache_access="<<ec.cache_access.value<<" assembly_timeout=10000000 read_window="<<ec.read_window.value
            <<" read_threshold="<<ec.read_threshold<<" bandwidth_GBps="<<ec.effective_gbytes_per_second
            <<" controls_B="<<ec.request_control_bytes<<"/"<<ec.response_control_bytes
            <<" propagation_ps="<<ec.request_fixed_latency.value<<"/"<<ec.response_fixed_latency.value
            <<" RX="<<ec.ingress.entries<<"/"<<ec.ingress.payload_bytes<<" TX="<<ec.egress.entries<<"/"<<ec.egress.payload_bytes
            <<" caches_per_bank=2 CoreChannel=ideal\n"
            <<"CONFIG retention_fragments="<<hc.retained_fragments<<" replay_pass_limit="<<hc.max_replay_passes
            <<" pending_retries="<<hc.max_pending_retries<<" timeout_retries="<<hc.max_timeout_retries<<" retry_delay="<<hc.pending_retry_delay.value
            <<" max_tick="<<limits.max_tick->value<<" event_limits="<<limits.events.total_events<<"/"
            <<limits.events.events_per_tick<<"/"<<limits.events.rounds_per_tick<<"\n"
            <<"STOP "<<to_string(report.status)<<" workload="<<(report.workload_success?"Success":"FailedOrUnfinished")
            <<" tick="<<report.at.value<<" events="<<report.events<<" reason="<<report.explanation<<"\n";
        for(auto& item:host.results()){auto&r=item.second;std::cout<<"GLOBAL "<<r.id.value<<" "<<result(r.state)
            <<" remaining="<<r.remaining<<" failed_fragments="<<r.failed<<" reason="<<r.reason<<"\n";}
        const auto&s=host.statistics();const auto&d=device.statistics();
        std::cout<<"HOST handed="<<s.handed<<" accepted="<<s.accepted<<" responses="<<s.responses
            <<" attempt_errors="<<s.attempt_errors<<" logical_success="<<s.logical_success<<" logical_failed="<<s.logical_failed
            <<" recovery_tasks="<<s.recovery_tasks<<" replay_passes="<<s.replay_passes<<" replay_commands="<<s.replay_commands
            <<" pending_limit_errors="<<s.pending_limit_errors<<" pending_retry_commands="<<s.pending_retry_commands
            <<" assembly_timeout_errors="<<s.assembly_timeout_errors<<" timeout_retry_rounds="<<s.timeout_retry_rounds
            <<" timeout_retry_commands="<<s.timeout_retry_commands<<" order_retry_commands="<<s.order_retry_commands<<"\n";
        for(auto&entry:host.recovery().tasks()){auto&t=entry.second;std::cout<<"RECOVERY id="<<t.id.value<<" channel="<<t.content.block.channel
            <<" block="<<t.content.block.block<<" content="<<t.content.content<<" through="<<(t.through?std::to_string(*t.through):"unknown")
            <<" passes="<<t.passes<<" state="<<(t.phase==RecoveryPhase::Completed?"Completed":t.phase==RecoveryPhase::Failed?"Failed":"Active")
            <<" reason="<<t.failure<<"\n";}
        std::cout<<"DEVICE programs="<<d.programs<<" erases="<<d.erases<<" senses="<<d.senses<<" cache_hits="<<d.cache_hits
            <<" host_drained="<<report.host.drained()<<" device_drained="<<report.device.drained()
            <<" pending_global="<<report.host.unfinished_global<<" source_queue="<<report.host.queued_units
            <<" unsettled_host="<<report.host.unsettled_attempts<<" outstanding_device="<<report.device.outstanding
            <<" ingress="<<report.device.ingress<<" pending_dlu="<<report.device.pending_dlus
            <<" bank_ops="<<report.device.bank_operations<<" egress="<<report.device.egress<<"\n";
        if(report.status==RunStatus::Completed)return report.workload_success?0:2;
        return report.status==RunStatus::Stalled?3:report.status==RunStatus::LimitReached?4:report.status==RunStatus::ModelError?5:6;
    }catch(const std::exception&e){std::cerr<<"INPUT/MODEL ERROR: "<<e.what()<<"\n";return 5;}
}

