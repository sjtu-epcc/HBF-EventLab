#include "hbflab/live_runtime.hpp"
#include "hbflab/async_device.cuh"
#include <iostream>
#include <set>
#include <tuple>
using namespace hbflab;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
void test(bool v,const char* s){if(!v)throw std::runtime_error(s);}
template<class T>__global__ void read_warp(AsyncDeviceView view,const T* source,T* out,unsigned mode) {
    const unsigned i=threadIdx.x,lane=i&31,warp=i/32;
    const unsigned mask=mode==1?0x00001fffu:mode==2?0x55555555u:0xffffffffu;
    if(!(mask&(1u<<lane)))return;
    const unsigned rank=__popc(mask&((1u<<lane)-1));
    unsigned index=warp*32+rank;
    if(mode==3 && lane==5)++index; // gap/overlap rejected, never broaden span
    if(mode==4)index+=4096/sizeof(T);
    T value{};AsyncDeviceClient client(view);
    const auto status=client.warp_load_contiguous(source+index,value,mask);
    if(status==Status::Ok)out[i]=value;
}
int main(int argc,char** argv)try {
    test(argc==2,"case required");std::string mode=argv[1];
    unsigned char data[4096];for(unsigned i=0;i<4096;++i)data[i]=(i*7+19)%251;
    void *input=nullptr,*output=nullptr;cudaStream_t stream=nullptr;
    ck(cudaMalloc(&input,4096));ck(cudaMalloc(&output,1024));ck(cudaMemcpy(input,data,4096,cudaMemcpyHostToDevice));
    ck(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
    const unsigned width=mode=="u8"?1:mode=="u16"?2:mode=="u64"?8:mode=="u128"?16:4;
    const void* kernel=width==1?reinterpret_cast<void*>(read_warp<unsigned char>):width==2?reinterpret_cast<void*>(read_warp<unsigned short>):
        width==8?reinterpret_cast<void*>(read_warp<unsigned long long>):width==16?reinterpret_cast<void*>(read_warp<uint4>):reinterpret_cast<void*>(read_warp<unsigned>);
    unsigned op=mode=="partial"?1:mode=="sparse"?2:mode=="layout"?3:mode=="range"?4:0;
    const unsigned active=op==1?13:op==2?16:32;
    const bool failure=op>=3;const unsigned loops=mode=="drain"?600:1;
    AsyncRuntimeOptions options;options.audit_limit=4;options.backend.max_events_per_batch=10000000;
    {
        LiveTimingService service(options,64,1);service.register_readonly(input,4096);service.start();
        std::set<std::tuple<std::uint64_t,std::uint32_t,std::uint64_t>> identities;
        std::uint64_t epoch=0;
        for(unsigned loop=0;loop<loops;++loop) {
            std::vector<void*> args{&input,&output,&op};service.launch({1,64},kernel,args,stream);
            if(failure)break;
            service.synchronize();auto audit=service.take_audit();test(audit.size()==2,"warp request count");
            test(service.take_audit().empty(),"audit drain repeats records");
            for(const auto& a:audit) {
                test(a.producer==0 || a.producer==32,"follower published request");
                test(a.first_bytes==active*width && a.span_count==1,"wrong union width");
                test(a.first_address==reinterpret_cast<uintptr_t>(input)+a.producer*width,"wrong union address");
                test(identities.emplace(a.launch,a.producer,a.ticket).second,"recycled identity");
            }
            const auto s=service.statistics();if(epoch)test(s.gpu_epoch_ns==epoch,"epoch reset");epoch=s.gpu_epoch_ns;
            unsigned char got[1024];ck(cudaMemcpy(got,output,64*width,cudaMemcpyDeviceToHost));
            for(unsigned i=0;i<64;++i) {
                const unsigned lane=i%32;if(op==1 && lane>=13)continue;if(op==2 && lane%2)continue;
                const unsigned rank=op==2?lane/2:lane;
                for(unsigned b=0;b<width;++b)test(got[i*width+b]==data[((i/32)*32+rank)*width+b],"typed load bytes mismatch");
            }
        }
        if(failure) {
            bool rejected=false;try{service.finish();}catch(...){rejected=true;}test(rejected,"invalid warp load accepted");
        }else {
            service.finish();const auto s=service.statistics();
            test(service.drained() && s.completed==loops*2 && s.submitted==s.completed && !s.audit_dropped,"drain or accounting");
            test(s.peak_inflight<=2 && s.launches_completed==loops,"bounded slots");
        }
    }
    ck(cudaFree(input));ck(cudaFree(output));ck(cudaStreamDestroy(stream));
    std::cout<<"warp "<<mode<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
