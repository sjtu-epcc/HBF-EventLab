#include "hbf_eventlab/configuration/simulation_configuration.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/adapters/workload_reader.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include "hbf_eventlab/statistics/event_stream.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
using namespace hbf_eventlab;
namespace {
bool same_path(const std::string& a,const std::string& b) {
    if(a.empty()||b.empty())return false;
    if(std::filesystem::exists(a)&&std::filesystem::exists(b))return std::filesystem::equivalent(a,b);
    auto x=std::filesystem::weakly_canonical(std::filesystem::absolute(a)).generic_string();
    auto y=std::filesystem::weakly_canonical(std::filesystem::absolute(b)).generic_string();
#ifdef _WIN32
    std::transform(x.begin(),x.end(),x.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    std::transform(y.begin(),y.end(),y.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
#endif
    return x==y;
}
void protect_output(const std::string& output,const std::string& workload,const std::string& config) {
    if(same_path(output,workload)||same_path(output,config))
        throw std::invalid_argument("output must not overwrite workload or input configuration");
}
void usage() {
    std::cout<<"usage: hbf_eventlab_cli workload.txt [--config FILE] [--set key=value] [--dump-config FILE]\n"
        <<"  [--channels N] [--interleave N] [--base N] [--burst] [--regular] [--open N]\n"
        <<"  [--mocs N] [--mocs-supported N] [--pending N] [--speed-grade 1|2|3]\n"
        <<"  [--replays N] [--retention N] [--pending-retries N] [--timeout-retries N] [--retry-delay N]\n"
        <<"  [--fail-sequence p,q] [--persistent-page N] [--max-tick N] [--max-events N]\n"
        <<"  [--trace] [--stats-json FILE] [--stats-events FILE]\n"
        <<"Defaults < INI file < explicit CLI overrides (left to right). See docs/parameters.md.\n";
}
const char* result(LogicalResult r){return r==LogicalResult::Success?"Success":r==LogicalResult::Failed?"Failed":"Pending";}
}
int main(int argc,char**argv){
    try{
        if(argc<2){usage();return 5;}
        if(std::string(argv[1])=="--help"){usage();return 0;}
        std::string config_path;
        std::vector<std::pair<std::string,std::string>> overrides;
        const std::map<std::string,std::string> aliases={
            {"--channels","device.geometry.host_channels"},{"--interleave","address.interleave_bytes"},
            {"--base","address.global_base"},{"--open","host.max_open_dlus_per_channel"},
            {"--replays","host.recovery.max_replay_passes"},{"--retention","host.retained_fragments"},
            {"--pending-retries","host.retry.max_pending_retries"},{"--timeout-retries","host.retry.max_timeout_retries"},
            {"--retry-delay","host.retry.pending_delay_ps"},{"--mocs","device.selected.mocs_selected"},
            {"--mocs-supported","device.capabilities.mocs_supported"},{"--pending","device.capabilities.max_pending_write_dlus"},
            {"--speed-grade","device.ucie_speed_grade"},{"--persistent-page","faults.persistent_page"},
            {"--max-tick","run.max_tick_ps"},{"--max-events","run.total_events"},
            {"--stats-json","output.statistics_json"},{"--stats-events","output.statistics_events_jsonl"},{"--dump-config","output.effective_config"}};
        for(int i=2;i<argc;++i){
            const std::string key=argv[i];
            if(key=="--burst"){overrides.emplace_back("host.write_mode","burst4096");continue;}
            if(key=="--regular"){overrides.emplace_back("host.read_hint_mode","all_regular");continue;}
            if(key=="--trace"){overrides.emplace_back("output.trace","true");continue;}
            if(key!="--config"&&key!="--set"&&key!="--fail-sequence"&&!aliases.count(key))
                throw std::invalid_argument("unknown option: "+key);
            if(i+1>=argc)throw std::invalid_argument("missing option value: "+key);
            const std::string value=argv[++i];
            if(key=="--config"){
                if(!config_path.empty()||value.empty())throw std::invalid_argument("exactly one nonempty --config path is supported");
                config_path=value;
            }else if(key=="--set"){
                const auto eq=value.find('=');if(eq==std::string::npos)throw std::invalid_argument("--set requires key=value");
                overrides.emplace_back(value.substr(0,eq),value.substr(eq+1));
            }else if(key=="--fail-sequence")overrides.emplace_back("faults.fail_sequence","["+value+"]");
            else overrides.emplace_back(aliases.at(key),value);
        }
        SimulationConfiguration raw;
        if(!config_path.empty()){
            std::ifstream file(config_path);if(!file)throw std::invalid_argument("cannot open configuration: "+config_path);
            raw=read_configuration(file,config_path);
        }
        for(const auto& o:overrides)apply_configuration_override(raw,o.first,o.second);
        const auto resolved=resolve_configuration(std::move(raw));
        const auto& cfg=resolved.values();const auto& hc=cfg.host;const auto& ec=cfg.execution;const auto& limits=cfg.run;
        const auto& geometry=cfg.device.geometry;
        const bool trace=cfg.output.trace;
        const std::optional<std::string> statistics_path=cfg.output.statistics_json.empty()?std::nullopt:std::optional<std::string>{cfg.output.statistics_json};
        protect_output(cfg.output.statistics_json,argv[1],config_path);
        protect_output(cfg.output.effective_config,argv[1],config_path);
        protect_output(cfg.output.statistics_events_jsonl,argv[1],config_path);
        if(same_path(cfg.output.statistics_events_jsonl,cfg.output.statistics_json)||same_path(cfg.output.statistics_events_jsonl,cfg.output.effective_config))
            throw std::invalid_argument("event stream output must differ from other outputs");
        if(same_path(cfg.output.statistics_json,cfg.output.effective_config))
            throw std::invalid_argument("statistics and effective configuration outputs must be different files");
        std::ifstream input(argv[1]);if(!input)throw std::invalid_argument("cannot open workload file");
        const auto workload=read_workload(input);
        StatisticsCollector measurements; // outlives both producers
        std::ofstream event_output;
        std::unique_ptr<StatisticsEventStream> event_stream;
        std::vector<IStatisticsSink*> sinks;
        if(statistics_path)sinks.push_back(&measurements);
        if(!cfg.output.statistics_events_jsonl.empty()) {
            event_output.open(cfg.output.statistics_events_jsonl,std::ios::binary);
            if(!event_output)throw std::invalid_argument("cannot open statistics event output");
            event_stream=std::make_unique<StatisticsEventStream>(event_output,cfg.output.statistics_buffer_bytes);sinks.push_back(event_stream.get());
        }
        StatisticsFanout observation(sinks);
        EventQueue events;DeviceDriver device(resolved.description(),ec,events);
        RoundRobinInterleaver inter(cfg.address.global_base,geometry.host_channels,geometry.channel_bytes(),cfg.address.interleave_bytes);
        HostDriver host(device,events,inter,hc);
        if(!sinks.empty()){device.set_statistics_sink(observation);host.set_statistics_sink(observation);}
        device.set_program_outcome(make_program_outcome(cfg.faults));
        if(trace){
            host.set_trace([](auto&e){std::cout<<"HOST "<<e.at.value<<" "<<e.action<<" cmd="<<e.command.value
                <<" unit="<<e.unit.value<<" channel="<<e.block.channel<<" block="<<e.block.block<<" pass="<<e.pass<<"\n";});
            device.set_trace([](auto&e){std::cout<<"DEVICE "<<e.at.value<<" "<<e.action<<" channel="<<e.channel<<" id="<<e.identity<<"\n";});
        }
        host.load(workload);
        for(const auto& diagnostic:diagnose_workload(resolved,workload))std::cerr<<"WARNING "<<diagnostic<<"\n";
        if(!cfg.output.effective_config.empty()){
            std::ofstream effective(cfg.output.effective_config);
            if(!effective)throw std::invalid_argument("cannot open effective configuration output");
            write_effective_configuration(effective,resolved);effective.flush();
            if(!effective)throw std::runtime_error("cannot flush effective configuration output");
        }
        std::ofstream statistics_output;
        if(statistics_path){
            statistics_output.open(*statistics_path);
            if(!statistics_output)throw std::invalid_argument("cannot open statistics output");
        }
        std::cout<<"HBF EventLab | functional timing, NOT calibrated performance\n";
        write_configuration_summary(std::cout,resolved);
        host.start();SimulationRunner runner(events,host,device);const auto report=runner.run(limits);
        if(event_stream)event_stream->finish(report.at,to_string(report.status));
        if(statistics_path) {
            measurements.write_json(statistics_output,report.at,to_string(report.status));statistics_output.flush();
            if(!statistics_output)throw std::runtime_error("cannot flush statistics output");
            std::cout<<"STATISTICS schema="<<StatisticsCollector::schema_version<<" file="<<*statistics_path<<" unit=tick (1 ps in CLI)\n";
        }
        std::cout            <<"STOP "<<to_string(report.status)<<" workload="<<(report.workload_success?"Success":"FailedOrUnfinished")
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

