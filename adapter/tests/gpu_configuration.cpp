#include "hbf_eventlab/gpu/configured_backend.hpp"
#include "hbf_eventlab/gpu/page_controller.hpp"
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::invalid_argument&){caught=true;}check(caught,"invalid configuration accepted");}
struct Outcome {std::uint64_t read_ps,programs,senses,reads,merges,accepted;};
Outcome exercise(ResolvedSimulationConfiguration c){
    EventQueue events;ConfiguredGpuBackend b(std::move(c),events);
    GpuControllerConfiguration gc;gc.sector_requests=true;gc.issue_width=32;gc.issue_limit=512;gc.mshrs=32;
    GpuPageController gpu(b.host,events,{},gc);b.host.start_incremental();gpu.start();
    std::uint64_t id=0;
    auto send=[&](Op op,std::uint64_t address,std::uint64_t bytes){
        GpuTraceRequest r;r.request={GlobalRequestId{++id},op,GlobalByteAddress{address},bytes,1};
        r.request.not_before=events.now();if(op==Op::Read)r.request.expected_content=1;gpu.enqueue(r);
    };
    auto drain=[&]{b.drain_events();check(gpu.success()&&gpu.drained()&&b.host.activity().drained()&&b.device.activity().drained(),"failed to drain");};
    // Several channels/banks and (in the compact fixture) consecutive pages in a block.
    for(unsigned p=0;p<16;++p){send(Op::Write,p*4096,4096);drain();}
    const auto start=events.now().value;
    for(unsigned p=0;p<16;++p){send(Op::Read,p*4096,32);send(Op::Read,p*4096+32,32);}
    drain();
    check(gpu.statistics().page_reads==16&&gpu.statistics().mshr_hits==16,"64B merging changed");
    check(gpu.statistics().completed==48,"completion mismatch");
    std::cout<<"SMOKE completed=48 reads64="<<gpu.statistics().page_reads<<" merges="<<gpu.statistics().mshr_hits
        <<" programs="<<b.device.statistics().programs<<" senses="<<b.device.statistics().senses
        <<" accepted="<<b.host.statistics().accepted<<" read_ps="<<events.now().value-start<<" drained=1\n";
    return {events.now().value-start,b.device.statistics().programs,b.device.statistics().senses,
        gpu.statistics().page_reads,gpu.statistics().mshr_hits,b.host.statistics().accepted};
}
int main(int argc,char** argv){try{
    check(argc>=3,"mode input.ini [effective.ini]");const std::string mode=argv[1];
    auto resolved=read_gpu_backend(argv[2]);auto c=resolved.values();
    if(mode=="experiment"){
        if(argc==4){std::ofstream out(argv[3]);write_effective_configuration(out,resolved);check(bool(out),"effective write failed");}
        write_configuration_summary(std::cout,resolved);exercise(std::move(resolved));return 0;
    }
    c.device.geometry={2,1,2,2,4,1};c.device.selected.zone_size_dlus=0;
    c.device.topology={};c.host.max_resident_fragments=128;
    if(mode=="mocs") {c.device.selected.mocs_selected=300;rejects([&]{resolve_gpu_backend(c);});}
    else if(mode=="supported") {c.device.capabilities.mocs_supported=256;c.device.selected.mocs_selected=512;rejects([&]{resolve_gpu_backend(c);});}
    else if(mode=="address") {c.address.interleave_bytes=64;rejects([&]{resolve_gpu_backend(c);});c.address.interleave_bytes=4096;c.address.global_base=GlobalByteAddress{4096};rejects([&]{resolve_gpu_backend(c);});}
    else if(mode=="output") {c.output.statistics_json="ignored.json";rejects([&]{resolve_gpu_backend(c);});}
    else if(mode=="unknown") {std::istringstream in("[host]\nsend_polcy=fcfs\n");rejects([&]{read_configuration(in);});}
    else if(mode=="roundtrip") {
        c.device.capabilities.mocs_supported=1024;c.device.selected.mocs_selected=512;c.device.capabilities.max_pending_write_dlus=7;
        c.host.send_policy="fill_open";c.execution.nand_scheduler="read_priority";
        auto a=resolve_gpu_backend(c);std::ostringstream x;write_effective_configuration(x,a);
        std::istringstream in(x.str());auto b=resolve_gpu_backend(read_configuration(in));std::ostringstream y;write_effective_configuration(y,b);
        check(x.str()==y.str(),"effective INI roundtrip mismatch");
        EventQueue e;ConfiguredGpuBackend runtime(std::move(b),e);
        check(runtime.configuration.description().configuration().mocs_selected==512 && runtime.configuration.description().capabilities().max_pending_write_dlus==7,"description lost selection");
        exercise(resolve_gpu_backend(c));
    }
    else if(mode=="timing") {
        c.execution.media_timing.sense=Tick{100000};auto a=exercise(resolve_gpu_backend(c));
        c.execution.media_timing.sense=Tick{1000000};auto b=exercise(resolve_gpu_backend(c));
        check(b.read_ps>a.read_ps&&a.senses==b.senses,"INI sense did not change runtime latency");
    }
    else if(mode=="write_mode") {
        auto a=exercise(resolve_gpu_backend(c));c.host.writes=WriteCommandMode::Fragment64;
        auto b=exercise(resolve_gpu_backend(c));check(b.accepted>a.accepted&&a.programs==b.programs,"write mode not applied");
    }
    else if(mode=="fault") {c.faults.fail_sequence={0};auto r=exercise(resolve_gpu_backend(c));check(r.programs>16,"fault injection not applied");}
    else if(mode=="clock") {
        EventQueue e;ConfiguredGpuBackend b(resolve_gpu_backend(c),e);unsigned calls=0;
        e.schedule_at(Tick{100},EventPhase::Arrival,[&]{++calls;});
        b.advance_open(Tick{100});check(!calls&&e.now()==Tick{100},"arrival endpoint was settled early");
        e.schedule_at(Tick{100},EventPhase::Arrival,[&]{++calls;});
        b.advance_open(Tick{101});check(calls==2,"same-tick inputs were lost");
    }
    else if(mode=="limits") {
        c.run.events.total_events=1;bool caught=false;try{exercise(resolve_gpu_backend(c));}catch(const EventLimitReached&){caught=true;}check(caught,"event budget ignored");
        c.run.events.total_events=0;c.run.max_tick=Tick{1};caught=false;try{exercise(resolve_gpu_backend(c));}catch(const EventLimitReached&){caught=true;}check(caught,"time budget ignored");
    }
    else throw std::runtime_error("unknown scenario");
    std::cout<<"PASS "<<mode<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
