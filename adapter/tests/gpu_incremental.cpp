#include "hbf_eventlab/gpu/page_controller.hpp"
#include "hbf_eventlab/gpu/page_interleaver.hpp"
#include "hbf_eventlab/runtime/device_driver.hpp"
#include <iostream>
using namespace hbf_eventlab;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
GpuTraceRequest request(std::uint64_t id,Op op,std::uint64_t address,std::uint64_t bytes=4096){
    GpuTraceRequest r;r.request={GlobalRequestId{id},op,GlobalByteAddress{address},bytes,1};
    if(op==Op::Read)r.request.expected_content=1;
    return r;
}
struct Fixture {
    EventQueue events;
    Geometry geometry{2,1,1,1,8,1};
    DeviceDriver device{{geometry,{256,8,false,true},{256,Tick{10000000},geometry.block_group_dlus()}},{},events};
    GpuPageInterleaver inter{2,geometry.channel_bytes()};
    HostDriver host;
    static HostConfiguration config(std::size_t budget,bool burst){HostConfiguration c;c.max_resident_fragments=budget;c.max_logical_fragments=0;if(burst)c.writes=WriteCommandMode::Burst4096;return c;}
    explicit Fixture(std::size_t budget=128,bool burst=false):host(device,events,inter,config(budget,burst)){}
    void start(GpuPageController& gpu){host.start_incremental();gpu.start();}
    void drain(GpuPageController& gpu){events.run();check(gpu.drained()&&host.activity().drained()&&device.activity().drained(),"actors did not drain");}
};
int main(int argc,char** argv){try{
    std::string mode=argc==2?argv[1]:"";
    const bool burst=mode.rfind("burst_",0)==0;
    if(burst)mode=mode.substr(6);
    Fixture f(mode=="too_large"?63:64,burst);
    GpuControllerConfiguration config;config.issue_width=8;config.issue_limit=64;config.sector_requests=true;
    if(mode=="recovery")f.device.set_program_outcome([failed=false](const ProgramOperation&)mutable{
        if(!failed){failed=true;return ProgramOutcome::FailedReusable;}return ProgramOutcome::Success;
    });
    std::vector<GpuTraceRequest> input;
    if(mode=="partial")input.push_back(request(1,Op::Write,0,32));
    else {input.push_back(request(1,Op::Write,0));input.push_back(request(2,Op::Write,4096));}
    GpuPageController gpu(f.host,f.events,input,config);f.start(gpu);
    if(mode=="too_large"){
        bool caught=false;try{f.events.run();}catch(const std::invalid_argument&){caught=true;}
        check(caught,"oversized page not rejected");check(f.host.results().empty(),"rejected page registered");
    }else{
        f.drain(gpu);
        if(mode=="partial")check(gpu.statistics().failed==1&&f.device.statistics().programs==0,"incomplete write fabricated");
        else{
            check(gpu.statistics().completed==2&&gpu.statistics().input_backpressure>0,"capacity rejection/retry not exercised");
            check(f.host.input_capacity().resident==0,"completed fragments not reclaimed");
            for(unsigned i=0;i<16;++i){auto r=request(3+i,Op::Read,i*32,32);r.request.not_before=f.events.now();gpu.enqueue(r);}
            f.drain(gpu);
            check(gpu.success()&&gpu.results().size()==18,"lost/duplicate GPU completion");
            check(gpu.statistics().page_reads==1&&gpu.statistics().mshr_hits==15,"cache lines were not merged into one page");
            check(f.host.results().size()==3,"backpressured input duplicated");
            if(mode=="recovery")check(f.host.statistics().replay_passes==1,"recovery bypassed");
            if(mode=="streaming"){
                for(unsigned i=0;i<64;++i){auto r=request(100+i,Op::Read,32,32);r.request.not_before=f.events.now();gpu.enqueue(r);f.drain(gpu);}
                check(gpu.statistics().completed==82&&f.host.input_capacity().resident==0,"streaming did not reclaim");
            }
        }
    }
    std::cout<<"PASS gpu_incremental_"<<mode<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
