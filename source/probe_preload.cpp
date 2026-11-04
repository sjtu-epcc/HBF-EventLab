// Diagnostic only: exercise the same incremental GPU page preload path.
// These wall times are NOT GPT-2 TTFT/TPOT.
#include "hbf_eventlab/gpu/page_controller.hpp"
#include "hbf_eventlab/gpu/page_interleaver.hpp"
#include "hbf_eventlab/runtime/device_driver.hpp"
#include "hbf_eventlab/diagnostics/runtime_profile.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace hbf_eventlab;
int main(int argc,char**argv){try{
  if(argc<3||argc>6)throw std::invalid_argument("pages profile.json [batch_pages] [burst4096] [read_batch]");
  const auto read_batch=argc==6?std::stoull(argv[5]):1;
  if(!read_batch||read_batch>64)throw std::invalid_argument("read batch must be 1..64");
  const auto count=std::stoull(argv[1]);
  const auto batch=argc>=4?std::stoull(argv[3]):16;
  if(!batch||batch>16)throw std::invalid_argument("batch must be 1..16");
  if(!count||count>4096)throw std::invalid_argument("pages must be 1..4096");
  RuntimeProfile profile;EventQueue events;
  events.set_limits({10000000,100000,100000});
  Geometry geometry{16,1,1,1,8192,1};
  ExecutionConfiguration execution;
  execution.media_timing.sense=Tick{15000000};
  execution.media_timing.program=Tick{200000000};
  execution.media_timing.erase=Tick{2000000000};
  execution.ucie.speed_grade=static_cast<UcieSpeedGrade>(3);
  DeviceDriver device{{geometry,{256,32,false,true},{256,Tick{1000000000},geometry.block_group_dlus()}},execution,events};
  GpuPageInterleaver inter{16,geometry.channel_bytes()};
  HostConfiguration hc;hc.max_open_dlus_per_channel=8;hc.max_logical_fragments=0;hc.max_resident_fragments=4096;
  if(argc>=5&&std::string(argv[4])=="burst4096")hc.writes=WriteCommandMode::Burst4096;
  HostDriver host(device,events,inter,hc);host.set_runtime_profile(profile);
  std::uint64_t fingerprint=1469598103934665603ULL;
  auto mix=[&](std::uint64_t x){for(unsigned j=0;j<8;++j){fingerprint^=(x>>(j*8))&255;fingerprint*=1099511628211ULL;}};
  host.set_trace([&](const HostTrace& t){for(auto p=t.action;*p;++p)mix(static_cast<unsigned char>(*p));mix(t.at.value);mix(t.command.value);mix(t.unit.value);mix(t.pass);});
  GpuControllerConfiguration gc;gc.cycle=Tick{710};gc.issue_width=32;gc.issue_limit=4352;gc.mshrs=32;gc.sector_requests=true;
  gc.write_buffers=32;gc.write_deadline=Tick{661376};
  GpuPageController controller(host,events,{},gc);
  host.start_incremental();controller.start();
  auto start=std::chrono::steady_clock::now();
  for(std::uint64_t page=0;page<count;++page){
    GpuTraceRequest r;r.source=UINT32_MAX;
    r.request={GlobalRequestId{page+1},Op::Write,GlobalByteAddress{page*4096},4096,page+1};
    r.request.not_before=events.now();controller.enqueue(r);
    if((page+1)%batch==0||page+1==count){
      events.run();
      if(!controller.drained()||!host.activity().drained()||!device.activity().drained()||!controller.success())throw std::runtime_error("preload did not drain successfully");
      const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
      std::cout<<"pages="<<page+1<<" wall_ns="<<ns<<" events="<<events.executed()<<" versions="<<host.storage().versions<<" programs="<<device.statistics().programs<<std::endl;
    }
  }
  const auto idle_start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<1000;++i)
    if(!controller.drained()||!host.activity().drained()||!device.activity().drained())throw std::runtime_error("idle check failed");
  std::cout<<"idle_checks=1000 wall_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-idle_start).count()<<std::endl;
  const auto read_start=std::chrono::steady_clock::now();
  const auto read_events=events.executed();
  events.set_limits({read_events+1000000,100000,100000});
  for(unsigned i=0;i<64;++i){
    GpuTraceRequest r;r.source=0;
    r.request={GlobalRequestId{count+1+i},Op::Read,GlobalByteAddress{(i%count)*4096},32,0};
    r.request.expected_content=i%count+1;r.request.not_before=events.now();controller.enqueue(r);
    if((i+1)%read_batch && i!=63)continue;
    try {events.run();} catch(const EventLimitReached& e) {
      std::cerr<<"READ_EVENT_LIMIT read="<<i<<" events="<<events.executed()-read_events<<" tick="<<events.now().value<<" reason="<<host.waiting_reason()<<" gpu="<<controller.waiting_reason()<<std::endl;
      std::ofstream partial(std::string(argv[2])+".partial");profile.write_json(partial);throw;
    }
    if(!controller.drained()||!host.activity().drained()||!device.activity().drained()||!controller.success())throw std::runtime_error("read failed");
  }
  std::cout<<"reads=64 wall_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-read_start).count()<<" events="<<events.executed()-read_events<<std::endl;
  std::cout<<"FUNCTIONAL fingerprint="<<fingerprint<<" tick="<<events.now().value<<" events="<<events.executed()<<" completed="<<controller.statistics().completed<<" failed="<<controller.statistics().failed<<" senses="<<device.statistics().senses<<" programs="<<device.statistics().programs<<std::endl;
  std::ofstream out(argv[2]);profile.write_json(out);
  if(!out)throw std::runtime_error("profile write failed");
  return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}}
