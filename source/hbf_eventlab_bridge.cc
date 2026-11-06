// EventLab backend for live Accel-Sim/GPGPU-Sim memory-system requests.
// The GPU remains responsible for functional instruction execution and bytes.
#include "hbf_eventlab_bridge.h"
#include "gpu-sim.h"
#include "mem_fetch.h"
#include "hbf_system.h"
#include "hbf_eventlab/gpu/page_controller.hpp"
#include "hbf_eventlab/gpu/page_interleaver.hpp"
#include "hbf_eventlab/runtime/device_driver.hpp"
#include "hbf_eventlab/configuration/simulation_configuration.hpp"
#include <cmath>
#include <chrono>
#include "hbf_eventlab/diagnostics/runtime_profile.hpp"
#include <cstdlib>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>

using namespace hbf_eventlab;
namespace {
std::uint64_t setting(const char *name, std::uint64_t fallback) {
  const char *value=std::getenv(name);
  if(!value)return fallback;
  std::string text(value);std::size_t end=0;
  if(text.empty()||text[0]=='-'||text[0]=='+')throw std::invalid_argument(name);
  auto result=std::stoull(text,&end,10);
  if(end!=text.size())throw std::invalid_argument(name);
  return result;
}
Tick clock_ticks(std::uint64_t cycles,double frequency) {
  const long double ticks=std::ceil(static_cast<long double>(cycles)*1.0e12L/frequency);
  if(!std::isfinite(ticks)||ticks<0||ticks>std::numeric_limits<std::uint64_t>::max())
    throw std::invalid_argument("GPU clock conversion overflow");
  return Tick{static_cast<std::uint64_t>(ticks)};
}
}

struct hbf_eventlab_bridge_t::implementation {
  struct Mapping {std::uint64_t address,content=0;GlobalRequestId image{};};
  const memory_config *config;
  gpgpu_sim *gpu;
  unsigned stacks,channels,total_channels;
  std::uint64_t pages_per_channel,limit,next_id=0,next_content=0;
  std::uint64_t admitted=0,returned=0,completed=0,failed=0,image_pages=0,seeded_pages=0;
  std::uint64_t max_owned=0,max_return_queue=0,sector_requests=0;
  Tick sampled{};
  long double epoch=0,last_seconds=-1;
  EventQueue events;
  std::unique_ptr<DeviceDriver> device;
  std::unique_ptr<GpuPageInterleaver> interleaver;
  std::unique_ptr<HostDriver> host;
  std::unique_ptr<GpuPageController> controller;
  std::map<std::uint64_t,Mapping> mapping;
  std::vector<std::uint64_t> next_slot;
  std::map<std::uint64_t,std::uint64_t> write_content;
  std::map<GlobalRequestId,mem_fetch *> owned;
  std::map<unsigned,std::deque<std::pair<GlobalRequestId,mem_fetch *>>> returns;
  std::ofstream trace;
  bool diag_enabled=std::getenv("HBF_DIAG_OUTPUT")!=nullptr;
  RuntimeProfile diagnostic_profile;
  using DiagClock=std::chrono::steady_clock;
  DiagClock::time_point diag_start=DiagClock::now(),diag_last=diag_start;
  std::uint64_t diag_advance_ns=0,diag_push_ns=0,diag_pop_ns=0,diag_busy_ns=0,diag_calls=0;
  bool image_dependencies_retired=false;
  void diagnostic_dump(const char* phase) {
    const auto now=DiagClock::now();
    const char* path=std::getenv("HBF_DIAG_OUTPUT");
    if(!path)return;
    std::ofstream out(path,std::ios::app);
    out<<"[DEBUG-hbf-perf] "<<phase<<" wall_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(now-diag_start).count()
       <<" advance_ns="<<diag_advance_ns<<" push_ns="<<diag_push_ns<<" pop_ns="<<diag_pop_ns<<" busy_ns="<<diag_busy_ns
       <<" advance_calls="<<diag_calls<<" events="<<events.executed()<<" admitted="<<admitted<<" completed="<<completed
       <<" returned="<<returned<<" owned="<<owned.size()<<" tick="<<events.now().value<<"\n";
    diagnostic_profile.write_json(out);out<<"\n";diag_last=now;
  }
  void diagnostic_periodic() {
    if((++diag_calls%4096)==0 && DiagClock::now()-diag_last>std::chrono::seconds(2))diagnostic_dump("sample");
  }

  implementation(const memory_config *c,gpgpu_sim *g):config(c),gpu(g),
      stacks(c->hbf_num_stacks),channels(c->hbf_num_channels),
      total_channels(stacks*channels),
      pages_per_channel(setting("HBF_EVENTLAB_PAGES_PER_CHANNEL",4096)),
      limit(c->hbf_max_outstanding) {
    if(!stacks||!channels||total_channels>16||c->hbf_page_size!=4096||!limit||
       !pages_per_channel||pages_per_channel>65536)
      throw std::invalid_argument("EventLab bridge requires 4KiB pages, 1..16 total channels, 1..65536 resident pages/channel");
    if(c->hbf_placement_mode==2)throw std::invalid_argument("explicit upstream placement table is not supported by EventLab bridge");
    // Use the backend's demonstrator lane geometry, scaled to the requested
    // per-channel residency. A backend INI may override every geometry field.
    if(pages_per_channel%512)
      throw std::invalid_argument("multi-bank geometry requires pages/channel divisible by 512");
    Geometry geometry{total_channels,4,16,8,pages_per_channel/512,2};
    if(const char *path=std::getenv("HBF_EVENTLAB_DEVICE_CONFIG")) {
      std::ifstream input(path);
      if(!input)throw std::invalid_argument("cannot open EventLab device configuration");
      geometry=read_configuration(input,path).device.geometry;
    }
    if(geometry.host_channels!=total_channels || geometry.channel_bytes()!=checked_mul(pages_per_channel,4096))
      throw std::invalid_argument("EventLab geometry must match GPU channel count and resident pages/channel");
    std::fprintf(stderr,"EVENTLAB_GEOMETRY channels=%u dies_per_channel=%u banks_per_die=%u pages_per_block=%u blocks_per_bank=%llu spare_blocks_per_bank=%u slots_per_bank=2\n",
      geometry.host_channels,geometry.dies_per_channel,geometry.banks_per_die,geometry.pages_per_block,
      static_cast<unsigned long long>(geometry.blocks_per_bank),geometry.spare_blocks_per_bank);
    StaticDeviceDescription description{geometry,{256,32,false,true},
      {256,Tick{1000000000},geometry.block_group_dlus()}};
    ExecutionConfiguration execution;
    const auto frequency=gpu->get_config().get_dram_freq();
    execution.media_timing.sense=clock_ticks(c->hbf_tR,frequency);
    execution.media_timing.program=clock_ticks(c->hbf_tPROG,frequency);
    execution.media_timing.erase=clock_ticks(c->hbf_tBERS,frequency);
    const auto grade=setting("HBF_EVENTLAB_UCIE_GRADE",3);
    if(grade<1||grade>3)throw std::invalid_argument("EventLab UCIe grade must be 1..3");
    execution.ucie.speed_grade=static_cast<UcieSpeedGrade>(grade);
    if(c->hbf_channel_bw_gbps!=execution.ucie.effective_gbytes_per_second())
      throw std::invalid_argument("GPU channel bandwidth must match latest EventLab UCIe grade (48/94/188 GB/s)");
    device=std::make_unique<DeviceDriver>(description,execution,events);
    interleaver=std::make_unique<GpuPageInterleaver>(total_channels,geometry.channel_bytes());
    HostConfiguration hc;hc.max_open_dlus_per_channel=8;
    const auto burst=setting("HBF_EVENTLAB_BURST_WRITES",0);
    if(burst>1)throw std::invalid_argument("HBF_EVENTLAB_BURST_WRITES must be 0 or 1");
    if(burst)hc.writes=WriteCommandMode::Burst4096;
    hc.max_logical_fragments=0;
    hc.max_resident_fragments=setting("HBF_EVENTLAB_RESIDENT_FRAGMENTS",4096);
    host=std::make_unique<HostDriver>(*device,events,*interleaver,hc);
    if(diag_enabled){host->set_runtime_profile(diagnostic_profile);device->set_runtime_profile(diagnostic_profile);}
    GpuControllerConfiguration gc;
    gc.cycle=clock_ticks(1,gpu->get_config().get_core_freq());
    gc.issue_width=total_channels*2;gc.issue_limit=checked_add(limit,256);
    gc.mshrs=setting("HBF_EVENTLAB_GPU_MSHRS",32);
    if(!gc.mshrs||gc.mshrs>limit)
      throw std::invalid_argument("HBF_EVENTLAB_GPU_MSHRS must be in 1..gpgpu_hbf_max_outstanding");
    gc.write_buffers=c->hbf_write_buffer_entries;
    gc.write_deadline=clock_ticks(c->hbf_write_agg_timeout,frequency);
    gc.merge_reads=c->hbf_mshr_enabled;gc.sector_requests=true;
    std::fprintf(stderr,"EVENTLAB_GPU_CONFIG mshr_units64=%zu issue_limit=%zu host_resident_fragments=%zu\n",
      gc.mshrs,gc.issue_limit,hc.max_resident_fragments);
    if(c->hbf_read_mode==1)gc.read_window=clock_ticks(c->hbf_read_agg_window,frequency);
    controller=std::make_unique<GpuPageController>(*host,events,std::vector<GpuTraceRequest>{},gc);
    controller->set_result_observer([this](GlobalRequestId id,const GpuRequestResult& result){finish(id,result);});
    host->start_incremental();
    controller->start();next_slot.resize(total_channels);
    if(const char *path=std::getenv("HBF_EVENTLAB_TRACE")) {
      trace.open(path);if(!trace)throw std::invalid_argument("cannot open EventLab lifecycle trace");
      trace<<"event,id,source,tick_ps,address,bytes,write\n";
    }
    // An explicit symbolic initial image is programmed through the normal
    // Host/Device path, not invented in response to an unwritten-page read.
    const auto initial=setting("HBF_EVENTLAB_IMAGE_PAGES",0);
    const auto image_capacity=checked_mul(pages_per_channel,total_channels);
    if(initial>image_capacity||initial>c->hbf_size/4096)
      throw std::invalid_argument("invalid initial image page count");
    std::set<std::uint64_t> initial_pages;
    for(std::uint64_t page=0;page<initial;++page)initial_pages.insert(page);
    if(const char *path=std::getenv("HBF_EVENTLAB_IMAGE_PAGE_FILE")) {
      std::ifstream input(path);
      if(!input)throw std::invalid_argument("cannot open EventLab image page file");
      std::string line;
      while(std::getline(input,line)) {
        const auto first=line.find_first_not_of(" \t\r");
        if(first==std::string::npos||line[first]=='#')continue;
        const auto last=line.find_last_not_of(" \t\r");
        const auto text=line.substr(first,last-first+1);std::size_t end=0;
        if(text[0]=='-'||text[0]=='+')throw std::invalid_argument("invalid EventLab image page");
        const auto base=text.rfind("0x",0)==0||text.rfind("0X",0)==0?16:10;
        const auto page=std::stoull(text,&end,base);
        if(end!=text.size())throw std::invalid_argument("invalid EventLab image page");
        initial_pages.insert(page);
      }
    }
    if(initial_pages.size()>image_capacity||(!initial_pages.empty()&&c->hbf_route_all))
      throw std::invalid_argument("invalid initial image page count/routing");
    const auto preload=setting("HBF_EVENTLAB_PRELOAD_IMAGE",1);
    if(preload>1)throw std::invalid_argument("HBF_EVENTLAB_PRELOAD_IMAGE must be 0 or 1");
    const auto seed_image=setting("HBF_EVENTLAB_SEED_IMAGE",0);
    if(seed_image)
      throw std::invalid_argument("incremental backend requires ordinary Host writes; SEED_IMAGE is unsupported");
    if(setting("HBF_EVENTLAB_FAIL_ONCE",0))
      device->set_program_outcome([used=false](const ProgramOperation&)mutable{
        if(!used){used=true;return ProgramOutcome::FailedReusable;}return ProgramOutcome::Success;
      });
    // Keep the streaming controller's ready queue bounded while constructing a
    // pre-existing model image. Enqueuing thousands of pages before the first
    // event jump makes the controller repeatedly scan a large blocked queue.
    constexpr std::size_t preload_batch_pages=16;
    std::fprintf(stderr,"EVENTLAB_PRELOAD begin pages=%llu write_mode=%s\n",
      static_cast<unsigned long long>(initial_pages.size()),burst?"burst4096":"fragment64");
    std::size_t preload_pending=0;
    std::map<std::tuple<std::uint64_t,std::uint64_t,std::uint64_t>,std::uint64_t> image_block_content;
    const auto drain_preload=[this]{
      events.run();
      const auto ha=host->activity();const auto da=device->activity();
      if(!controller->drained()||!ha.drained()||!da.drained()) {
        std::fprintf(stderr,"EVENTLAB_PRELOAD stalled after=%llu controller=%d host_unfinished=%zu host_queued=%zu device_pending=%zu device_bank_ops=%zu reason=%s\n",
          static_cast<unsigned long long>(image_pages),controller->drained(),ha.unfinished_global,ha.queued_units,
          da.pending_dlus,da.bank_operations,controller->waiting_reason().c_str());
        throw std::logic_error("EventLab image preload did not drain");
      }
    };
    for(const auto page:initial_pages) {
      if(page>=c->hbf_size/4096)throw std::invalid_argument("initial image page outside HBF capacity");
      auto& slot=map_page(page);
      const auto global_slot=slot.address/4096;
      const auto channel=global_slot%total_channels;
      const auto local_slot=global_slot/total_channels;
      const auto block=local_slot/geometry.block_group_dlus();
      const auto lane=local_slot%geometry.lanes();
      auto [content,created]=image_block_content.try_emplace({channel,block,lane},0);
      if(created)content->second=++next_content;
      slot.content=content->second;
      slot.image=GlobalRequestId{++next_id};
      GpuTraceRequest r;r.source=UINT32_MAX;
      r.request={slot.image,Op::Write,GlobalByteAddress{slot.address},4096,slot.content};
      r.request.not_before=events.now();controller->enqueue(r);++image_pages;
      if(preload&&++preload_pending==preload_batch_pages) {
        drain_preload();preload_pending=0;
        if(image_pages%4096==0)std::fprintf(stderr,"EVENTLAB_PRELOAD completed_pages=%llu total_pages=%llu\n",
          static_cast<unsigned long long>(image_pages),static_cast<unsigned long long>(initial_pages.size()));
      }
    }
    if(preload) {
      // Model images exist before timed GPU execution. Still use the complete
      // Host/Device/NAND path, but settle it with EventLab's event jumps rather
      // than burning GPU cycles and tripping the core deadlock detector.
      if(preload_pending)drain_preload();
      sampled=events.now();epoch=static_cast<long double>(sampled.value)/1.0e12L;
    }
    std::fprintf(stderr,"HBF backend=EventLab | live GPU requests | image_pages=%llu seeded_pages=%llu | symbolic data | configured geometry\n",
                 static_cast<unsigned long long>(image_pages),static_cast<unsigned long long>(seeded_pages));
    diagnostic_dump("preload_done");
  }
  Mapping& map_page(std::uint64_t page) {
    const auto found=mapping.find(page);if(found!=mapping.end())return found->second;
    const auto location=hbf_system_t::map_relative_address(page*4096,config->hbf_size,4096,stacks,config->hbf_stack_map);
    const auto per_stack_pages=config->hbf_size/4096/stacks;
    const unsigned channel=location.stack*channels+
      hbf_sim_import::hbf_page_to_channel(channels,config->hbf_placement_mode,location.local_page,per_stack_pages);
    auto& next=next_slot.at(channel);
    if(next>=pages_per_channel)throw std::runtime_error("EventLab sparse residency capacity exceeded; no address wrapping");
    const auto address=checked_mul(checked_add(checked_mul(next++,total_channels),channel),4096);
    return mapping.emplace(page,Mapping{address}).first->second;
  }
  std::uint64_t logical(mem_fetch *mf) const {
    if(mf->isatomic())throw std::invalid_argument("EventLab bridge does not support GPU atomics");
    return hbf_system_t::logical_address_for_mapping(mf->get_addr(),config->hbf_base_addr,config->hbf_route_all);
  }
  void log(const char *event,GlobalRequestId id,mem_fetch *mf,Tick at) {
    if(trace)trace<<event<<','<<id.value<<','<<mf->get_sub_partition_id()<<','<<at.value<<','
      <<mf->get_addr()<<','<<mf->get_data_size()<<','<<mf->is_write()<<'\n';
  }
  void push(mem_fetch *mf) {
    if(owned.size()>=limit)throw std::logic_error("GPU admission exceeded bridge ownership limit");
    const auto relative=logical(mf),page=relative/4096;
    if(!config->hbf_route_all&&relative>=config->hbf_size)throw std::invalid_argument("GPU HBF address outside capacity");
    auto& slot=map_page(page);
    GpuTraceRequest r;r.source=mf->get_sub_partition_id();
    const GlobalRequestId id{++next_id};
    r.request={id,mf->is_write()?Op::Write:Op::Read,GlobalByteAddress{slot.address+relative%4096},
      mf->get_data_size(),slot.content,{}, {},events.now()};
    r.byte_mask=mf->get_access_byte_mask();
    if(slot.image.value)r.request.after.push_back(slot.image);
    if(mf->is_write()) {
      auto batch=write_content.find(page);
      if(batch==write_content.end())batch=write_content.emplace(page,++next_content).first;
      slot.content=batch->second;r.request.content=slot.content;
    }else if(slot.content)r.request.expected_content=slot.content;
    owned.emplace(id,mf);++admitted;if(mf->get_data_size()==32)++sector_requests;
    max_owned=std::max<std::uint64_t>(max_owned,owned.size());log("INGRESS",id,mf,events.now());
    controller->enqueue(std::move(r));
  }
  void finish(GlobalRequestId id,const GpuRequestResult& result) {
    auto item=owned.find(id);
    if(!result.success) {
      ++failed;std::ostringstream reason;
      reason<<"EventLab final failure id="<<id.value<<" reason="<<result.reason;
      if(item!=owned.end())log("FAILED",id,item->second,result.completed);
      // mem_fetch has no error response protocol. Never fake a successful reply.
      throw std::runtime_error(reason.str());
    }
    if(item==owned.end())return; // explicit image transaction, not a GPU mf
    auto *mf=item->second;++completed;
    if(mf->is_write())write_content.erase(logical(mf)/4096);
    if(mf->get_access_type()!=L1_WRBK_ACC&&mf->get_access_type()!=L2_WRBK_ACC)mf->set_reply();
    returns[mf->get_sub_partition_id()].push_back({id,mf});
    std::size_t queued=0;for(const auto& source:returns)queued+=source.second.size();
    max_return_queue=std::max<std::uint64_t>(max_return_queue,queued);
    log("COMPLETED",id,mf,result.completed);
  }
  void retire_history() {
    if(!owned.empty()||!controller->drained()||!host->activity().drained()||!device->activity().drained())return;
    if(!image_dependencies_retired) {
      for(auto& item:mapping)item.second.image={};
      image_dependencies_retired=true;
    }
    const auto removed=controller->retire_completed_history();
    if(removed)std::fprintf(stderr,"EVENTLAB_HISTORY_RETIRED requests=%zu cumulative_completed=%llu\n",
      removed,static_cast<unsigned long long>(controller->statistics().completed));
  }
  void advance(double seconds) {
    if(!std::isfinite(seconds)||seconds<0)throw std::invalid_argument("invalid GPU time");
    // GPGPU-Sim resets domain clocks between kernels. Preserve a monotonic
    // EventLab epoch; its prior kernel must have drained before this reset.
    if(last_seconds>=0&&seconds<last_seconds) {
      if(!owned.empty()||!controller->drained()||!host->activity().drained()||!device->activity().drained())
        throw std::logic_error("GPU clock reset with in-flight HBF work");
      retire_history();
      epoch+=last_seconds+1.0L/gpu->get_config().get_dram_freq();
    }
    last_seconds=seconds;
    const long double ps=(epoch+seconds)*1.0e12L;
    if(ps>std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("GPU timestamp overflow");
    sampled=Tick{static_cast<std::uint64_t>(std::floor(ps))};
    // Leave the sampled endpoint open for this GPU cycle's arrivals. This
    // preserves the latest EventQueue's normal/deadline settlement semantics.
    while(const auto next=events.next_time()) {
      if(!(*next<sampled))break;
      events.run_next();
    }
    events.advance_idle_to(sampled);
  }
  mem_fetch *pop(unsigned source) {
    auto found=returns.find(source);if(found==returns.end()||found->second.empty())return nullptr;
    auto result=found->second.front();found->second.pop_front();
    log("RETURNED",result.first,result.second,sampled);
    owned.erase(result.first);++returned;
    if(found->second.empty())returns.erase(found);
    return result.second;
  }
  bool busy() const {return !owned.empty()||!controller->drained()||!host->activity().drained()||!device->activity().drained();}
  void print(FILE *fp) {
    // Accel-Sim normally reports at each kernel boundary while its clock is
    // cumulative. Retire there as well as on a clock reset.
    retire_history();
    const auto& c=controller->statistics();const auto& d=device->statistics();const auto& h=host->statistics();
    std::fprintf(fp,"\n========= EventLab Live GPU Backend =========\n");
    std::fprintf(fp,"EVENTLAB_GPU admitted=%llu completed=%llu returned=%llu failed=%llu owned=%zu sector32=%llu max_owned=%llu max_return_queue=%llu\n",
      (unsigned long long)admitted,(unsigned long long)completed,(unsigned long long)returned,(unsigned long long)failed,
      owned.size(),(unsigned long long)sector_requests,(unsigned long long)max_owned,(unsigned long long)max_return_queue);
    std::fprintf(fp,"EVENTLAB_PAGE image_pages=%llu seeded_pages=%llu reads=%llu writes=%llu mshr_hits=%llu mapped=%zu incomplete=%llu\n",
      (unsigned long long)image_pages,(unsigned long long)seeded_pages,(unsigned long long)c.page_reads,(unsigned long long)c.page_writes,
      (unsigned long long)c.mshr_hits,mapping.size(),(unsigned long long)c.incomplete_writes);
    std::fprintf(fp,"EVENTLAB_DEVICE senses=%llu programs=%llu erases=%llu replay_passes=%llu attempt_errors=%llu tick_ps=%llu drained=%d\n",
      (unsigned long long)d.senses,(unsigned long long)d.programs,(unsigned long long)d.erases,(unsigned long long)h.replay_passes,
      (unsigned long long)h.attempt_errors,(unsigned long long)sampled.value,!busy());
    if(trace)trace.flush();
  }
};

hbf_eventlab_bridge_t::hbf_eventlab_bridge_t(const memory_config *c,gpgpu_sim *g):state(new implementation(c,g)){}
hbf_eventlab_bridge_t::~hbf_eventlab_bridge_t(){delete state;}
bool hbf_eventlab_bridge_t::full()const{return state->owned.size()>=state->limit;}
void hbf_eventlab_bridge_t::push(mem_fetch *mf){if(!state->diag_enabled){state->push(mf);return;}const auto t=implementation::DiagClock::now();state->push(mf);state->diag_push_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(implementation::DiagClock::now()-t).count();}
void hbf_eventlab_bridge_t::advance(double seconds){if(!state->diag_enabled){state->advance(seconds);return;}const auto t=implementation::DiagClock::now();state->advance(seconds);state->diag_advance_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(implementation::DiagClock::now()-t).count();state->diagnostic_periodic();}
mem_fetch *hbf_eventlab_bridge_t::pop_return_for(unsigned source){if(!state->diag_enabled)return state->pop(source);const auto t=implementation::DiagClock::now();auto* r=state->pop(source);state->diag_pop_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(implementation::DiagClock::now()-t).count();return r;}
bool hbf_eventlab_bridge_t::busy()const{if(!state->diag_enabled)return state->busy();const auto t=implementation::DiagClock::now();const bool r=state->busy();state->diag_busy_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(implementation::DiagClock::now()-t).count();return r;}
unsigned hbf_eventlab_bridge_t::channels_per_stack()const{return state->channels;}
void hbf_eventlab_bridge_t::print_stat(FILE *fp){state->print(fp);}
