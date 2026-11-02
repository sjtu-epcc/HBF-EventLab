#include "hbflab/async_runtime.hpp"
#include "hbflab/async_device.cuh"
#include <algorithm>
#include <iostream>
#include <string>
#include <thread>
#include <tuple>
using namespace hbflab;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool yes=false;try{f();}catch(const std::exception&){yes=true;}check(yes,"expected rejection");}
struct Result {unsigned value=0,count=0;Status status=Status::Ok;AsyncAccessTrace trace{};};
struct alignas(16) Four {unsigned a,b,c,d;};
__global__ void reader(AsyncDeviceView view,const unsigned* input,Result* results,unsigned mode,unsigned bias) {
    const auto id=blockIdx.x*blockDim.x+threadIdx.x;
    if(mode==9)return;
    if(mode==1 && id%3==0)return; // No collective / no empty frontier required.
    AsyncDeviceClient client(view);Result r{};
    const unsigned rounds=mode==1?id%5+1:mode==8?1:2;
    for(unsigned i=0;i<rounds;++i) {
        const auto until=gpu_now_ns()+(id%7)*1000;
        while(gpu_now_ns()<until)__nanosleep(64);
        if(mode==2) {
            unsigned char bytes[17]{};
            r.status=client.read_span(reinterpret_cast<const unsigned char*>(input)+4090,bytes,17,&r.trace);
            if(r.status==Status::Ok)r.value=bytes[0]+bytes[16];
        }else if(mode==10) {
            Four value{};r.status=client.load(reinterpret_cast<const Four*>(input)+id%512,value,&r.trace);
            r.value=value.a+value.b+value.c+value.d;
        }else r.status=client.load(input+(mode==3?4096:(id+bias)%2048),r.value,&r.trace);
        if(r.status!=Status::Ok)break;++r.count;
    }
    results[id]=r;
}
__global__ void corrupt(AsyncDeviceView v,const unsigned*,Result*,unsigned mode,unsigned) {
    auto& m=v.mailboxes[0];
    if(mode==4){release(m.want_sequence,std::uint64_t{2});return;}
    release(m.want_sequence,std::uint64_t{1});
    const auto start=gpu_now_ns();
    while(acquire(m.grant_sequence)!=1 && !acquire(m.terminal) && gpu_now_ns()-start<v.timeout_ns)__nanosleep(128);
    if(acquire(m.terminal))return;
    if(mode==5)return; // abandoned grant
    while(gpu_now_ns()<m.floor_ns)__nanosleep(128);
    AsyncRequest r{};r.session=v.session+(mode==6);r.launch=v.launch;r.read.producer=v.producer_base;
    r.read.ticket=1;r.read.arrival_ns=mode==7?m.floor_ns-1:gpu_now_ns();r.read.span_count=1;
    r.read.spans[0]={v.ranges[0].gpu_base,4,0};m.request=r;release(m.request_sequence,std::uint64_t{1});
    while(!acquire(m.terminal) && acquire(m.done_sequence)!=1 && gpu_now_ns()-start<v.timeout_ns)__nanosleep(128);
}
int main(int argc,char** argv){try{
    const std::string name=argc>1?argv[1]:"";
    AsyncRuntimeOptions options;options.timeout_ns=1000000000;
    if(name=="scaled")options.time_scale=7;
    if(name=="pressure"){options.backend.outstanding=1;options.backend.resident_fragments=64;options.backend.timing.buffers.ingress={1,4096};options.backend.timing.buffers.egress={1,64};options.backend.timing.media_timing.sense=hbf_eventlab::Tick{1000000000};}
    if(name=="audit")options.audit_limit=3;
    if(name=="backend_error")options.backend.max_events_per_batch=100;
    if(name=="no_service")options.timeout_ns=10000000;
    AsyncRuntime runtime(options);
    std::vector<unsigned> input(2048);for(unsigned i=0;i<input.size();++i)input[i]=i*17+3;
    const auto* device=static_cast<const unsigned*>(runtime.upload(input.data(),input.size()*sizeof(unsigned)));
    unsigned blocks=4,threads=8;
    if(name=="pressure")threads=32; // exceed the explicit 64-fragment Host input budget
    if(name=="oversubscribed") {
        int sms=0,per_sm=0;ck(cudaDeviceGetAttribute(&sms,cudaDevAttrMultiProcessorCount,0));
        ck(cudaDeviceGetAttribute(&per_sm,cudaDevAttrMaxBlocksPerMultiprocessor,0));
        blocks=sms*per_sm+17;threads=1;
        check(blocks<=8192,"oversubscription proof exceeds configured producer bound on this GPU");
        std::cout<<"ctas="<<blocks<<" resident_upper_bound="<<sms*per_sm<<'\n';
    }
    const bool multi=name=="kernels" || name=="serial" || name=="abandon";
    unsigned mode=name=="divergent"?1:name=="span"?2:name=="invalid"?3:0;
    if(name=="ticket")mode=4;if(name=="abandon")mode=5;if(name=="identity")mode=6;if(name=="backdate")mode=7;
    if(name=="oversubscribed")mode=8;if(name=="empty")mode=9;if(name=="typed")mode=10;
    const bool bad=mode>=4 && mode<=7;
    if(bad){blocks=1;threads=1;}
    const auto count=blocks*threads;
    Result* results=nullptr;ck(cudaMalloc(reinterpret_cast<void**>(&results),count*(multi?2:1)*sizeof(Result)));
    ck(cudaMemset(results,0,count*(multi?2:1)*sizeof(Result)));
    cudaStream_t a=nullptr,b=nullptr;ck(cudaStreamCreateWithFlags(&a,cudaStreamNonBlocking));ck(cudaStreamCreateWithFlags(&b,cudaStreamNonBlocking));
    std::vector<AsyncLaunchShape> shapes{{blocks,threads}};if(multi)shapes.push_back({blocks,threads});
    if(name=="validation") {
        rejects([&]{runtime.prepare({{9000,1}});});
        rejects([&]{runtime.launch(0,reinterpret_cast<const void*>(reader),{},a);});
    }
    runtime.prepare(shapes);
    if(name=="validation") {
        rejects([&]{runtime.prepare(shapes);});rejects([&]{runtime.upload(input.data(),4);});
        rejects([&]{runtime.serve();});
        bool rejected=false;std::thread other([&]{try{runtime.serve();}catch(...){rejected=true;}});other.join();check(rejected,"owner host thread");
    }
    unsigned bias=0;const auto kernel=bad?reinterpret_cast<const void*>(corrupt):reinterpret_cast<const void*>(reader);
    runtime.launch(0,kernel,{&device,&results,&mode,&bias},a);
    if(name=="validation")rejects([&]{runtime.launch(0,kernel,{&device,&results,&mode,&bias},a);});
    if(multi){
        auto* out=results+count;bias=19;unsigned second_mode=name=="abandon"?0:mode;
        runtime.launch(1,reinterpret_cast<const void*>(reader),{&device,&out,&second_mode,&bias},name=="serial"?a:b);
    }
    if(name=="no_service")ck(cudaStreamSynchronize(a));
    const auto status=runtime.serve();
    std::vector<Result> actual(count*(multi?2:1));ck(cudaMemcpy(actual.data(),results,actual.size()*sizeof(Result),cudaMemcpyDeviceToHost));
    const bool failure=name=="invalid" || bad || name=="no_service" || name=="backend_error";
    if(failure) {
        const auto expected=name=="invalid"?Status::InvalidRange:name=="no_service"?Status::Timeout:name=="backend_error"?Status::BackendError:Status::ProtocolError;
        check(status==expected,"expected terminal service failure");check(!runtime.drained(),"failed session remains poisoned");
    }else {
        check(status==Status::Ok && runtime.drained(),"successful asynchronous drain");std::uint64_t expected=0;
        for(unsigned n=0;n<actual.size();++n) {
            const auto id=n%count;const auto& r=actual[n];
            if(mode==9 || (mode==1 && id%3==0)){check(r.count==0,"inactive thread untouched");continue;}
            const auto rounds=mode==1?id%5+1:mode==8?1:2;expected+=rounds;
            check(r.status==Status::Ok && r.count==rounds,"per producer request count");
            const auto bytes=reinterpret_cast<const unsigned char*>(input.data());
            const auto offset=(id%512)*4;
            const auto value=mode==2?unsigned(bytes[4090]+bytes[4106]):mode==10?
                input[offset]+input[offset+1]+input[offset+2]+input[offset+3]:input[(id+(n>=count?19:0))%2048];
            check(r.value==value,"real GPU byte authority");
            check(r.trace.access.arrival_ns>=r.trace.floor_ns && r.trace.access.arrival_ns>=r.trace.granted_ns &&
                  r.trace.granted_ns>=r.trace.wanted_ns,"admission/timestamp order");
            check(r.trace.access.resumed_ns-r.trace.access.arrival_ns>=r.trace.access.delay_ns &&
                  r.trace.access.resumed_ns>=r.trace.access.completion_seen_ns,"independent modeled completion gate");
        }
        const auto stats=runtime.statistics();check(stats.grants==expected && stats.submitted==expected && stats.completed==expected,"exactly once accounting");
        check(stats.backend.peak_registered_globals<=options.backend.resident_fragments,"global child window");
        if(name=="audit")check(runtime.audit().size()==3 && stats.audit_dropped==expected-3,"bounded audit");
        if(multi){bool first=false,second=false;for(const auto& r:runtime.audit()){first|=r.launch==1;second|=r.launch==2;}check(first && second,"distinct kernel identities");}
        if(name=="pressure")check(stats.backend.backpressure>0,"real backend backpressure");
        auto audit=runtime.audit();std::sort(audit.begin(),audit.end(),[](const auto& a,const auto& b){return a.dispatch<b.dispatch;});
        for(unsigned i=0;i<audit.size();++i) {
            const auto& r=audit[i];check(r.arrival_ns>=r.grant_floor_ns,"host verifies admission floor");
            if(i)check(std::tie(audit[i-1].arrival_ns,audit[i-1].producer,audit[i-1].ticket)<std::tie(r.arrival_ns,r.producer,r.ticket),"global deterministic dispatch order");
        }
        std::cout<<"requests="<<stats.completed<<" peak_parents="<<stats.peak_inflight<<" peak_registered_globals="<<stats.backend.peak_registered_globals<<" peak_active_launches="<<stats.peak_active_launches<<'\n';
    }
    ck(cudaFree(results));ck(cudaStreamDestroy(a));ck(cudaStreamDestroy(b));
    std::cout<<"PASS "<<name<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
