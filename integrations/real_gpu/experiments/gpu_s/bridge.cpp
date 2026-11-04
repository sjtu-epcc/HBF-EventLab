#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
using namespace hbf_eventlab;
using json=nlohmann::ordered_json;
namespace fs=std::filesystem;
static void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static SimulationConfiguration config(std::size_t resident=256){SimulationConfiguration c;c.device.geometry={1,1,2,16,64,1};c.device.capabilities={256,4,false,true};c.device.selected={256,Tick{10000000},c.device.geometry.block_group_dlus()};c.host.writes=WriteCommandMode::Burst4096;c.host.hints=ReadHintMode::AllRegular;c.host.max_resident_fragments=resident;c.run.max_tick=std::nullopt;c.run.events={10000000,100000,100000};return c;}
static int run(int argc,char** argv){
    need(argc==2 || argc==3,"Usage: gpu_s_bridge output_dir [preflight]");fs::path out=argv[1];need(!fs::exists(out),"Output exists");fs::create_directories(out);
    auto resolved=resolve_configuration(config());std::ofstream ini(out/"effective-H0.ini");write_effective_configuration(ini,resolved);ini.close();
    if(argc==3){need(std::string(argv[2])=="preflight","Unknown mode");std::ofstream(out/"preflight.json")<<json({{"status","pass"},{"channel_bytes",config().device.geometry.channel_bytes()},{"local_dlu_bytes",4096},{"chunk_bytes",4096},{"resident_fragments",256},{"max_events",10000000},{"capacity_check",16384<=config().device.geometry.channel_bytes()},{"io_submitted",0}}).dump(2);return 0;}
    StatisticsCollector stats;TensorAccessBridge bridge(resolved,{{"A",GlobalByteAddress{0},8192,1},{"B",GlobalByteAddress{8192},8192,1}}, {}, &stats);
    std::ofstream trace(out/"bridge.jsonl");bridge.set_trace([&](const TensorBridgeTrace& e){trace<<json({{"action",e.action},{"access",e.access},{"global",e.global.value},{"tick_ps",e.at.value}}).dump()<<'\n';});
    auto a=bridge.initialize("A"),b=bridge.initialize("B");need(a.success&&b.success,"Initialization failed");
    json results=json::array();std::uint64_t total=0,read_globals=0;
    for(auto name:{"A","A","B"}){const std::uint64_t bytes=std::strcmp(name,"B")==0?4096:8192;const auto r=bridge.access(name,0,bytes);need(r.complete&&r.success&&r.completed.has_value(),"Read failed");
        need(r.finished_globals==r.registered_globals,"Unfinished child");results.push_back({{"region",name},{"offset",0},{"bytes",bytes},{"access",r.id},{"offered_ps",r.offered.value},{"completed_ps",r.completed->value},{"globals",r.registered_globals},{"success",r.success}});total+=bytes;read_globals+=r.registered_globals;}
    auto final=bridge.finish();need(final.run.status==RunStatus::Completed&&final.run.workload_success&&final.run.host.drained()&&final.run.device.drained(),"Bridge failed final drain");need(read_globals==5 && total==20480,"Independent expected work mismatch");
    std::ofstream sf(out/"statistics.json");stats.write_json(sf,bridge.session().now(),to_string(final.run.status));
    json negative=json::array();
    auto rejects=[&](const char* name,auto fn){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}need(rejected,name);negative.push_back({{"case",name},{"rejected",rejected}});};
    rejects("overlapping_global",[&]{TensorAccessBridge x(resolve_configuration(config()),{{"A",GlobalByteAddress{0},8192,1},{"B",GlobalByteAddress{4096},8192,1}});});
    rejects("misaligned",[&]{TensorAccessBridge x(resolve_configuration(config()),{{"A",GlobalByteAddress{1},8192,1}});});
    rejects("capacity",[&]{TensorAccessBridge x(resolve_configuration(config()),{{"A",GlobalByteAddress{0},config().device.geometry.channel_bytes()+4096,1}});});
    rejects("content_conflict",[&]{TensorAccessBridge x(resolve_configuration(config()),{{"A",GlobalByteAddress{0},4096,1},{"B",GlobalByteAddress{8192},4096,2}});});
    TensorAccessBridge offset(resolve_configuration(config()),{{"A",GlobalByteAddress{0},8192,1}});need(offset.initialize("A").success,"Offset initialization failed");auto r=offset.access("A",64,128);need(r.success,"Nonzero offset read failed");const auto at=offset.session().now();need(offset.advance_compute(Tick{0}).success&&offset.session().now()==at,"Zero compute inserted time");need(offset.advance_compute(Tick{123456}).success&&offset.session().now().value==at.value+123456,"Compute-only tail incorrect");auto offset_finish=offset.finish();need(offset_finish.run.workload_success&&offset_finish.run.host.drained()&&offset_finish.run.device.drained(),"Offset final failure");
    TensorAccessBridge pressure(resolve_configuration(config(64)),{{"A",GlobalByteAddress{0},16384,1}});auto init=pressure.initialize("A");need(init.success,"Pressure initialization failed");auto p=pressure.access("A",0,16384);need(p.success&&p.capacity_rejections>0,"Pressure not exercised");auto pressure_finish=pressure.finish();need(pressure_finish.run.workload_success&&pressure_finish.run.host.drained()&&pressure_finish.run.device.drained(),"Pressure failed drain");
    auto f=config();f.faults.persistent_page=0;TensorAccessBridge fault(resolve_configuration(f),{{"A",GlobalByteAddress{0},8192,1}});auto failure=fault.initialize("A");need(!failure.success,"Program failure misreported success");auto failed_finish=fault.finish();need(failed_finish.run.host.drained()&&failed_finish.run.device.drained(),"Normal failure did not drain");
    json summary={{"status","pass"},{"reads",results},{"registered_valid_bytes",16384},{"touched_union_bytes",12288},{"cumulative_read_bytes",total},{"read_globals",read_globals},{"registered_globals",stats.globals().size()},{"input_closed",final.input_closed},{"host_drained",final.run.host.drained()},{"device_drained",final.run.device.drained()},{"pressure",{{"resident_fragments",64},{"rejections",p.capacity_rejections},{"success",p.success}}},{"program_fail",{{"success",failure.success},{"reason",failure.reason},{"host_drained",failed_finish.run.host.drained()},{"device_drained",failed_finish.run.device.drained()}}},{"rejected",negative}};
    summary["offset_zero_tail"]={{"success",r.success},{"offered_ps",r.offered.value},{"completed_ps",r.completed->value},{"registered_globals",r.registered_globals},{"finished_globals",r.finished_globals},{"offset",64},{"bytes",128},{"zero_compute_preserved_tick",true},{"tail_compute_ps",123456},{"input_closed",offset_finish.input_closed},{"host_drained",offset_finish.run.host.drained()},{"device_drained",offset_finish.run.device.drained()}};
    summary["pressure"].update({{"offered_ps",p.offered.value},{"completed_ps",p.completed->value},{"registered_globals",p.registered_globals},{"finished_globals",p.finished_globals},{"input_closed",pressure_finish.input_closed},{"host_drained",pressure_finish.run.host.drained()},{"device_drained",pressure_finish.run.device.drained()}});
    summary["program_fail"].update({{"input_closed",failed_finish.input_closed},{"registered_globals",failure.registered_globals},{"finished_globals",failure.finished_globals},{"run_status",to_string(failed_finish.run.status)}});
    std::ofstream(out/"summary.json")<<summary.dump(2);std::cout<<"S1 real bridge cases passed\n";return 0;
}
int main(int argc,char** argv){try{return run(argc,argv);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
