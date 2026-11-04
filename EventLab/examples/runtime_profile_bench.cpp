// Deterministic CPU producer: same original Host/Device path as the tensor bridge.
// No CUDA, payload bytes, per-command logging, or performance pass/fail threshold.
#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace hbf_eventlab;
int main(int argc,char**argv){try{
    if(argc<3||argc>4)throw std::invalid_argument("usage: hbf_eventlab_runtime_bench PAGES READ_PASSES [PROFILE_JSON]");
    auto number=[](const std::string& s){if(s.empty()||s.find_first_not_of("0123456789")!=std::string::npos)
        throw std::invalid_argument("expected unsigned decimal");return std::stoull(s);};
    const auto pages=number(argv[1]),passes=number(argv[2]);
    if(!pages||pages>4096||passes>128)throw std::invalid_argument("benchmark bounds: pages 1..4096, read passes 0..128");
    const auto bytes=pages*dlu_bytes;
    SimulationConfiguration c;c.device.geometry={1,1,2,32,(pages+63)/64,1};
    c.host.max_resident_fragments=64;c.host.max_logical_fragments=0;c.run.events.total_events=0;
    std::unique_ptr<RuntimeProfile> profile;if(argc==4)profile=std::make_unique<RuntimeProfile>();
    std::ofstream output;if(profile){output.open(argv[3]);if(!output)throw std::invalid_argument("cannot open runtime profile");}
    TensorAccessBridge bridge(resolve_configuration(c),{{"tensor",GlobalByteAddress{},bytes,1}},{},nullptr,profile.get());
    using Clock=std::chrono::steady_clock;const auto start=Clock::now();
    const auto init=bridge.initialize("tensor");if(!init.success)throw std::runtime_error(init.reason);
    for(std::uint64_t i=0;i<passes;++i){const auto r=bridge.access("tensor",0,bytes,Tick{1000});if(!r.success)throw std::runtime_error(r.reason);}
    const auto report=bridge.finish();const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
    if(profile){profile->write_json(output);output.close();if(!output)throw std::runtime_error("profile close failed");}
    const auto& h=bridge.session().host().statistics();const auto& d=bridge.session().device().statistics();
    const bool ok=report.run.status==RunStatus::Completed&&report.run.workload_success&&report.run.host.drained()&&report.run.device.drained();
    std::cout<<"{\"success\":"<<(ok?"true":"false")<<",\"program_wall_ns\":"<<ns<<",\"final_tick\":"<<report.run.at.value
        <<",\"events\":"<<report.run.events<<",\"globals\":"<<bridge.session().host().results().size()<<",\"responses\":"<<h.responses
        <<",\"programs\":"<<d.programs<<",\"senses\":"<<d.senses<<",\"cache_hits\":"<<d.cache_hits<<"}\n";
    return ok?0:2;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
