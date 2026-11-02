#include "hbflab/live_runtime.hpp"
#include "hbflab/async_device.cuh"
#include <cuda.h>
#include <iostream>
#include <thread>
#include <set>
using namespace hbflab;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
void test(bool x,const char* text){if(!x)throw std::runtime_error(text);}
template<class F>void reject(F f){bool caught=false;try{f();}catch(...){caught=true;}test(caught,"expected rejection");}
__global__ void live_read(AsyncDeviceView v,const unsigned* source,unsigned* out,unsigned mode) {
    const unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
    if(mode==2) {
        if(i==0){auto& m=v.mailboxes[0];release(m.want_sequence,acquire(m.want_sequence)+1);}
        return;
    }
    AsyncDeviceClient c(v);unsigned value=0;
    const auto status=c.load(source+(mode==1?1024:i%64),value);
    out[i]=status==Status::Ok?value:0xdeadbeef;
}
int main(int argc,char** argv)try {
    test(argc==2,"case required");std::string mode=argv[1];
    unsigned *input=nullptr,*output=nullptr;cudaStream_t a=nullptr,b=nullptr;
    ck(cudaMalloc(&input,4096));ck(cudaMalloc(&output,128*sizeof(unsigned)));
    unsigned values[1024];for(unsigned i=0;i<1024;++i)values[i]=i+17;
    ck(cudaMemcpy(input,values,sizeof(values),cudaMemcpyHostToDevice));
    ck(cudaStreamCreateWithFlags(&a,cudaStreamNonBlocking));ck(cudaStreamCreateWithFlags(&b,cudaStreamNonBlocking));
    AsyncRuntimeOptions options;options.audit_limit=10000;options.backend.max_events_per_batch=1000000;
    if(mode=="budget")options.backend.max_events_per_batch=100;
    {
        LiveTimingService service(options,128,2);
        if(mode=="borrow") {
            reject([&]{service.register_readonly(values,sizeof(values));});
            reject([&]{service.register_readonly(input+1020,32);});
            service.register_readonly(input,256);
            reject([&]{service.register_readonly(input+8,64);});
        }else service.register_readonly(input,sizeof(values));
        service.start();
        reject([&]{service.register_readonly(input,sizeof(values));});
        if(mode=="owner") {
            bool refused=false;std::thread other([&]{try{service.statistics();}catch(...){refused=true;}});other.join();
            test(refused,"owner check");
        }
        if(mode=="capture") {
            ck(cudaStreamBeginCapture(a,cudaStreamCaptureModeThreadLocal));
            unsigned op=0;std::vector<void*> args{&input,&output,&op};
            reject([&]{service.launch({1,64},reinterpret_cast<void*>(live_read),args,a);});
            cudaGraph_t graph=nullptr;ck(cudaStreamEndCapture(a,&graph));if(graph)ck(cudaGraphDestroy(graph));
        }
        const bool failure=mode=="invalid" || mode=="abandon" || mode=="budget";
        const unsigned loops=mode=="reuse"?40:mode=="gap"?4:mode=="empty"?0:1;
        std::uint64_t epoch=0,last_tick=0;
        for(unsigned j=0;j<loops;++j) {
            unsigned op=mode=="invalid"?1:mode=="abandon"?2:0;
            auto* dest=output+(j%2)*64;
            std::vector<void*> args{&input,&dest,&op};
            service.launch({2,32},reinterpret_cast<void*>(live_read),args,j%2?a:b);
            if(mode=="gap") {
                service.synchronize();const auto stats=service.statistics();
                if(epoch)test(epoch==stats.gpu_epoch_ns && stats.model_now_ps>last_tick,"idle session clock reset");
                epoch=stats.gpu_epoch_ns;last_tick=stats.model_now_ps;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if(failure)reject([&]{service.finish();});
        else {
            service.finish();test(service.drained(),"live service not drained");
            const auto stats=service.statistics();
            test(stats.launches_submitted==loops && stats.launches_completed==loops,"launch accounting");
            test(stats.submitted==loops*64 && stats.completed==stats.submitted,"request accounting");
            test(stats.model_now_ps>=stats.model_epoch_ps && stats.gpu_epoch_ns,"continuous clock");
            auto audit=service.audit();std::set<std::uint64_t> ids;
            for(const auto& r:audit)ids.insert(r.launch);
            test(ids.size()==loops,"launch generations reused");
            unsigned got[128];ck(cudaMemcpy(got,output,sizeof(got),cudaMemcpyDeviceToHost));
            if(loops)for(unsigned i=0;i<64;++i)test(got[i]==values[i],"borrowed bytes mismatch");
            reject([&]{unsigned op=0;service.launch({1,1},reinterpret_cast<void*>(live_read),{&input,&output,&op},a);});
        }
    }
    unsigned after[1024];ck(cudaMemcpy(after,input,sizeof(after),cudaMemcpyDeviceToHost));
    test(std::equal(std::begin(values),std::end(values),std::begin(after)),"borrowed input changed/freed");
    ck(cudaFree(input));ck(cudaFree(output));ck(cudaStreamDestroy(a));ck(cudaStreamDestroy(b));
    std::cout<<"live "<<mode<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
