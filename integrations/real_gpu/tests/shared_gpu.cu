#include "hbflab/live_runtime.hpp"
#include "hbflab/async_device.cuh"
#include "hbflab/capacity_gpu.hpp"
#include "hbflab/frontier.hpp"
#include "hbflab/kv.hpp"
#include <iostream>
#include <map>
#include <tuple>
#include <thread>
#include <string>
using namespace hbflab;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
void test(bool x,const char* s){if(!x)throw std::runtime_error(s);}
template<class F>void reject(F f){bool caught=false;try{f();}catch(...){caught=true;}test(caught,"expected rejection");}
__global__ void weight_reads(AsyncDeviceView view,const uint4* input,unsigned* result,unsigned loops) {
    for(unsigned j=0;j<loops;++j) {
        uint4 value{};AsyncDeviceClient c(view);
        if(c.warp_load_contiguous(input+threadIdx.x,value,0xffffffffu)!=Status::Ok){result[threadIdx.x]=0;return;}
        result[threadIdx.x]=value.x;
    }
}
__global__ void page_write(unsigned* p,unsigned value){p[threadIdx.x]=value+threadIdx.x;}
__global__ void page_read(const unsigned* p,unsigned* out){out[threadIdx.x]=p[threadIdx.x];}
int main(int argc,char** argv)try {
    test(argc==2,"case required");std::string mode=argv[1];
    uint4* weights=nullptr;unsigned *output=nullptr,*page_output=nullptr;
    ck(cudaMalloc(&weights,4096));ck(cudaMalloc(&output,128));ck(cudaMalloc(&page_output,128));
    uint4 data[256];for(unsigned i=0;i<256;++i)data[i]=make_uint4(100+i,200+i,300+i,400+i);
    ck(cudaMemcpy(weights,data,4096,cudaMemcpyHostToDevice));
    cudaStream_t a,b;ck(cudaStreamCreateWithFlags(&a,cudaStreamNonBlocking));ck(cudaStreamCreateWithFlags(&b,cudaStreamNonBlocking));
    AsyncRuntimeOptions o;o.audit_limit=16384;o.backend.max_events_per_batch=10000000;
    if(mode=="scaled")o.time_scale=3;
    if(mode=="failure")o.backend.fail_program_after=5;
    if(mode=="pressure"){o.backend.resident_fragments=64;o.backend.outstanding=1;o.backend.timing.buffers.ingress={1,4096};o.backend.timing.buffers.egress={1,64};}
    {
        LiveTimingService service(o,32,1);service.register_readonly(weights,4096);
        CapacityOptions options;options.logical_pages=4;options.frames=1;
        CapacityGpu capacity(options,&service);auto allocation=capacity.store().allocate(4*4096);
        test(service.media_statistics().device.programs==5,"single allocation namespace initialization");
        service.start();reject([&]{service.reserve_media(4096);});reject([&]{service.read_media(0,4);});
        if(mode=="owner") {
            bool refused=false;std::thread t([&]{try{service.read_media(4096,4);}catch(...){refused=true;}});t.join();test(refused,"foreign media owner accepted");
        }
        if(mode=="frame_guard") {
            auto lease=capacity.acquire(allocation,0,PageAccess::Read,b);
            auto* frame=static_cast<const uint4*>(capacity.data(lease));unsigned loops=1;
            std::vector<void*> args{&frame,&output,&loops};service.launch({1,32},reinterpret_cast<void*>(weight_reads),args,a);
            reject([&]{service.synchronize();});ck(cudaStreamSynchronize(a));capacity.finish(lease);
        }else {
            unsigned loops=mode=="pressure"?128:512;
            std::vector<void*> args{&weights,&output,&loops};service.launch({1,32},reinterpret_cast<void*>(weight_reads),args,a);
            if(mode=="abort") {
                capacity.store().abort();reject([&]{service.synchronize();});ck(cudaStreamSynchronize(a));
            }else if(mode=="kv") {
                capacity.store().release(allocation);
                KvOptions ko;ko.layers=1;ko.heads=1;ko.head_dimension=8;ko.element_bytes=4;ko.tokens_per_block=1;ko.max_tokens=2;ko.max_sequences=1;
                KvCache cache(capacity.store(),ko);auto sequence=cache.create();
                unsigned k[8],v[8],actual[8];
                for(unsigned token=0;token<2;++token) {
                    for(unsigned i=0;i<8;++i){k[i]=token*100+i;v[i]=token*100+50+i;}
                    cache.append(sequence,k,v,sizeof(k));
                }
                cache.flush(sequence);
                for(unsigned token=0;token<2;++token)for(auto kind:{KvKind::Key,KvKind::Value}) {
                    cache.read(sequence,0,token,kind,actual,sizeof(actual));
                    for(unsigned i=0;i<8;++i)test(actual[i]==token*100+(kind==KvKind::Key?0:50)+i,"shared KV bytes mismatch");
                }
                test(cache.length(sequence)==2 && !cache.poisoned(),"KV publication failed");cache.release(sequence);
                service.synchronize();service.finish();const auto s=service.statistics();
                test(service.drained() && s.submitted==loops && s.media_completed>0 && s.peak_gpu_during_media>0,"KV did not share GPU session");
                test(capacity.store().statistics().allocated_pages==0,"KV pages leaked");
            }else {
                auto first=capacity.acquire(allocation,0,PageAccess::Write,b);
                page_write<<<1,32,0,b>>>(static_cast<unsigned*>(capacity.writable_data(first)),42);ck(cudaGetLastError());capacity.finish(first);
                if(mode=="failure") {
                    reject([&]{capacity.acquire(allocation,1,PageAccess::Read,b);});
                    const auto failed=capacity.store().statistics();test(failed.poisoned && failed.dirty_frames==1,"failed writeback lost authoritative bytes");
                    reject([&]{service.synchronize();});ck(cudaStreamSynchronize(a));
                }else {
                    auto second=capacity.acquire(allocation,1,PageAccess::Write,b);
                    page_write<<<1,32,0,b>>>(static_cast<unsigned*>(capacity.writable_data(second)),900);ck(cudaGetLastError());capacity.finish(second);
                    auto again=capacity.acquire(allocation,0,PageAccess::Read,b);
                    page_read<<<1,32,0,b>>>(static_cast<const unsigned*>(capacity.data(again)),page_output);ck(cudaGetLastError());capacity.finish(again);
                    unsigned got[32];ck(cudaMemcpy(got,page_output,128,cudaMemcpyDeviceToHost));
                    for(unsigned i=0;i<32;++i)test(got[i]==42+i,"dirty eviction/reload bytes mismatch");
                    auto hit=capacity.acquire(allocation,0,PageAccess::Read,b);capacity.finish(hit);
                    capacity.flush();service.synchronize();service.finish();
                    const auto stats=service.statistics();const auto cs=capacity.store().statistics();
                    test(service.drained() && cs.pins==0 && cs.dirty_frames==0,"shared device did not drain");
                    test(stats.submitted==loops && stats.completed==loops && stats.media_submitted==5 && stats.media_completed==5,"GPU/media counters");
                    test(stats.peak_gpu_during_media>0,"no GPU/media overlap exercised");
                    test(stats.backend.requests==loops+3 && stats.backend.sectors==loops*8+3*64,"duplicate page-in charge");
                    test(stats.backend.writebacks==2 && cs.writebacks==2 && cs.faults==3 && cs.hits==1,"capacity lifecycle mismatch");
                    test(stats.backend.device.accepted==stats.backend.device.delivered && stats.backend.device.accepted==5+stats.backend.sectors+stats.backend.replayed_pages,"shared command lifecycle mismatch");
                    std::map<std::uint64_t,std::tuple<std::uint64_t,std::uint32_t,std::uint64_t>> ordered;
                    for(const auto& x:service.take_audit())test(ordered.emplace(x.dispatch,std::make_tuple(x.arrival_ns,x.producer,x.ticket)).second,"duplicate GPU dispatch");
                    auto media=service.take_media_audit();test(media.size()==5,"media audit lost");
                    for(const auto& x:media) {
                        test(x.producer==32 && x.arrival_ns>=x.floor_ns && x.end_ps>x.arrival_ps,"host media ownership/time");
                        test(x.resumed_wall_ns>=x.completion_wall_ns && x.completion_wall_ns>=x.ready_wall_ns &&
                             x.resumed_wall_ns-x.ready_wall_ns>=x.delay_ns,"host page consumed before completion/deadline");
                        test(x.arrival_ps==stats.model_epoch_ps+scale_ticks(x.arrival_ns-stats.gpu_epoch_ns,1000,o.time_scale),"media used different epoch");
                        test(ordered.emplace(x.dispatch,std::make_tuple(x.arrival_ns,x.producer,x.ticket)).second,"duplicate media dispatch");
                    }
                    std::tuple<std::uint64_t,std::uint32_t,std::uint64_t> previous{};std::uint64_t dispatch=0;
                    for(const auto& x:ordered){test(x.first==++dispatch && previous<=x.second,"cross-source arrival ordering");previous=x.second;}
                    test(dispatch==loops+5 && !stats.audit_dropped,"audit counts");
                    ck(cudaMemcpy(got,output,128,cudaMemcpyDeviceToHost));for(unsigned i=0;i<32;++i)test(got[i]==100+i,"borrowed weight corrupted by replay");
                    std::cout<<"gpu_requests="<<stats.completed<<" media_requests="<<stats.media_completed<<" overlap="<<stats.peak_gpu_during_media
                             <<" dependencies="<<stats.backend.dependency_waits<<" sectors="<<stats.backend.sectors<<" replayed_pages="<<stats.backend.replayed_pages<<'\n';
                    capacity.store().release(allocation);
                }
            }
        }
    }
    ck(cudaStreamDestroy(a));ck(cudaStreamDestroy(b));ck(cudaFree(weights));ck(cudaFree(output));ck(cudaFree(page_output));
    std::cout<<"shared GPU "<<mode<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
