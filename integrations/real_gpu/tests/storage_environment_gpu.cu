#include "hbflab/live_runtime.hpp"
#include "hbflab/async_device.cuh"
#include "hbflab/capacity_gpu.hpp"
#include "ptx_fixture.hpp"
#include <chrono>
#include <iostream>
#include <map>
#include <tuple>
using namespace hbflab;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
__global__ void read_weight(AsyncDeviceView view,const unsigned* w,unsigned* out) {
    AsyncDeviceClient client(view);for(unsigned i=0;i<32;++i){unsigned v=0;if(client.load(w+threadIdx.x,v)!=Status::Ok)return;out[threadIdx.x]=v;}
}
__global__ void update_page(unsigned* p){for(unsigned i=threadIdx.x;i<1024;i+=blockDim.x)p[i]=i+731;}
__global__ void check_page(const unsigned* p,unsigned* errors){for(unsigned i=threadIdx.x;i<1024;i+=blockDim.x)if(p[i]!=i+731)atomicAdd(errors,1u);}
int main()try {
    const auto path=std::filesystem::temp_directory_path()/("hbflab-gpu-storage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pages");
    struct Cleanup {std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove(p,ec);}} cleanup{path};
    unsigned *weights=nullptr,*out=nullptr,*errors=nullptr;ck(cudaMalloc(&weights,4096));ck(cudaMalloc(&out,128));ck(cudaMalloc(&errors,4));
    unsigned w[1024];for(unsigned i=0;i<1024;++i)w[i]=i+51;ck(cudaMemcpy(weights,w,4096,cudaMemcpyHostToDevice));ck(cudaMemset(errors,0,4));
    cudaStream_t stream;ck(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
    CapacityOptions o;o.logical_pages=8;o.frames=2;o.backing_mode=BackingMode::CreateFile;o.backing_path=path;
    o.backend.geometry={1,1,1,2,8,1};o.environment.enabled=true;o.environment.retention_seconds=1e-8;
    o.environment.initial_c=o.environment.ambient_c=o.environment.reference_c=25;
    // Accelerated retention exercises the scheduler; not a physical calibration.
    {
        AsyncRuntimeOptions options;options.backend=o.backend;options.audit_limit=8192;
        LiveTimingService service(options,32,1);service.register_readonly(weights,4096);
        CapacityGpu gpu(o,&service);const auto a=gpu.store().allocate(4*4096);service.start();
        std::vector<void*> args{&weights,&out};service.launch({1,32},reinterpret_cast<void*>(read_weight),args,stream);
        require(gpu.prefetch(a,0,2)==2,"file pages prefetched on shared service");
        require(gpu.statistics().prefetch_uploads==2,"prefetch fills actual CUDA frames");
        auto lease=gpu.acquire(a,0,PageAccess::Write);update_page<<<1,128>>>(static_cast<unsigned*>(gpu.writable_data(lease)));ck(cudaGetLastError());gpu.finish(lease);
        for(unsigned p=2;p<4;++p){lease=gpu.acquire(a,p,PageAccess::Read);gpu.finish(lease);}
        require(gpu.maintain(100)==1,"refresh serviced through shared owner");
        lease=gpu.acquire(a,0,PageAccess::Read);check_page<<<1,128>>>(static_cast<const unsigned*>(gpu.data(lease)),errors);ck(cudaGetLastError());gpu.finish(lease);
        gpu.flush();service.synchronize();service.finish();
        const auto s=gpu.store().statistics();const auto r=service.statistics();
        require(s.file_read_bytes && s.file_write_bytes && s.refreshes==1 && s.prefetch_useful>0,"features all exercised");
        require(r.completed==1024 && r.submitted==1024 && r.media_completed==r.media_submitted && service.drained(),"combined session drained");
        require(s.backend.requests==r.backend.requests && s.backend.replayed_pages==r.backend.replayed_pages,"single shared resource accounting");
        std::map<std::uint64_t,std::tuple<std::uint64_t,std::uint32_t,std::uint64_t>> ordered;
        for(const auto& x:service.take_audit())require(ordered.emplace(x.dispatch,std::make_tuple(x.arrival_ns,x.producer,x.ticket)).second,"GPU dispatch identity");
        for(const auto& x:service.take_media_audit()) {
            require(x.producer==32 && x.arrival_ns>=x.floor_ns && x.end_ps>x.arrival_ps,"maintenance uses host producer admission");
            require(x.resumed_wall_ns>=x.completion_wall_ns && x.resumed_wall_ns-x.ready_wall_ns>=x.delay_ns,"media waits until completion");
            require(ordered.emplace(x.dispatch,std::make_tuple(x.arrival_ns,x.producer,x.ticket)).second,"media dispatch identity");
        }
        std::uint64_t dispatch=0;std::tuple<std::uint64_t,std::uint32_t,std::uint64_t> previous{};
        for(const auto& x:ordered){require(x.first==++dispatch && previous<=x.second,"cross-source arrival ordering");previous=x.second;}
        require(dispatch==r.completed+r.media_completed && !r.audit_dropped,"complete combined trace");
        std::cout<<"gpu_requests="<<r.completed<<" media_requests="<<r.media_completed<<" refreshes="<<s.refreshes<<" prefetch_useful="<<s.prefetch_useful<<" temperature_c="<<s.environment.temperature_c<<'\n';
    }
    o.backing_mode=BackingMode::OpenFile;
    {CapacityGpu gpu(o);const auto restored=gpu.store().allocations();require(restored.size()==1,"GPU file allocation restored");
     const auto lease=gpu.acquire(restored[0],0,PageAccess::Read);check_page<<<1,128>>>(static_cast<const unsigned*>(gpu.data(lease)),errors);ck(cudaGetLastError());gpu.finish(lease);}
    unsigned count=1;ck(cudaMemcpy(&count,errors,4,cudaMemcpyDeviceToHost));require(!count,"real GPU file/refresh byte oracle");
    unsigned values[32];ck(cudaMemcpy(values,out,128,cudaMemcpyDeviceToHost));for(unsigned i=0;i<32;++i)require(values[i]==i+51,"weight bytes preserved through shared block replay");
    ck(cudaStreamDestroy(stream));ck(cudaFree(weights));ck(cudaFree(out));ck(cudaFree(errors));
    std::cout<<"storage environment GPU passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
