// Normal Host initialization and incremental feedback, no NAND seeding or GPU.
// Increase pages/channel to expose notification fan-out under concurrent reads.
#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "hbf_eventlab/statistics/event_stream.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace hbf_eventlab;
int main(int argc,char** argv) { try {
    if(argc<4 || argc>6)throw std::invalid_argument("usage: hbf_eventlab_read_wakeup_bench CHANNELS PAGES_PER_CHANNEL READ_PASSES [PROFILE_JSON|-] [EVENTS_JSONL]");
    auto number=[](std::string s){if(s.empty()||s.find_first_not_of("0123456789")!=std::string::npos)throw std::invalid_argument("unsigned integer expected");return std::stoull(s);};
    const auto channels=number(argv[1]),pages=number(argv[2]),passes=number(argv[3]);
    if(!channels||channels>16||!pages||pages>16||!passes||passes>128)throw std::invalid_argument("bounds: channels 1..16, pages 1..16, passes 1..128");
    SimulationConfiguration c;c.device.geometry={static_cast<std::uint32_t>(channels),1,1,8,(pages+7)/8,1};
    c.device.capabilities.mocs_supported=1024;c.device.selected.mocs_selected=1024;
    c.host.hints=ReadHintMode::AllRegular;c.execution.read_dispatch.threshold=0;c.execution.read_dispatch.wait=Tick{10000000};
    const auto bytes=channels*pages*dlu_bytes;
    c.host.max_resident_fragments=bytes/host_unit_bytes;c.host.max_logical_fragments=0;c.run.events.total_events=0;
    std::unique_ptr<RuntimeProfile> profile;
    if(argc>=5 && std::string(argv[4])!="-")profile=std::make_unique<RuntimeProfile>();
    std::ofstream event_file;std::unique_ptr<StatisticsEventStream> stream;
    if(argc==6){event_file.open(argv[5]);if(!event_file)throw std::runtime_error("cannot open events");stream=std::make_unique<StatisticsEventStream>(event_file,65536);}
    SimulationSession s(resolve_configuration(c),stream.get(),profile.get());
    s.set_completion_handler([&](const GlobalCompletion& done){
        if(done.result.state!=LogicalResult::Success)throw std::runtime_error("workload failed");
        if(done.result.id.value<=passes) {
            GlobalRequest r{GlobalRequestId{done.result.id.value+1},Op::Read,GlobalByteAddress{},bytes};
            r.not_before=s.now();r.expected_content=1;s.submit_feedback(r);
        }
    });
    const auto start=std::chrono::steady_clock::now();
    s.submit(GlobalRequest{GlobalRequestId{1},Op::Write,GlobalByteAddress{},bytes,1});s.close_input();
    const auto result=s.run_to_completion();
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    if(stream)stream->finish(result.run.at,to_string(result.run.status));
    if(profile){std::ofstream out(argv[4]);if(!out)throw std::runtime_error("cannot open profile");profile->write_json(out);}
    const auto& h=s.host().statistics();const auto& d=s.device().statistics();
    const bool ok=result.run.status==RunStatus::Completed&&result.run.workload_success&&result.run.host.drained()&&result.run.device.drained();
    std::cout<<"{\"success\":"<<(ok?"true":"false")<<",\"program_wall_ns\":"<<ns<<",\"final_tick\":"<<result.run.at.value
        <<",\"events\":"<<result.run.events<<",\"responses\":"<<h.responses<<",\"programs\":"<<d.programs
        <<",\"senses\":"<<d.senses<<",\"cache_hits\":"<<d.cache_hits<<"}\n";
    return ok?0:2;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} }
