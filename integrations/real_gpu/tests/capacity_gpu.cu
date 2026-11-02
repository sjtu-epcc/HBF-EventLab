#include "hbflab/capacity_gpu.hpp"
#include "hbflab/kv.hpp"
#include "ptx_fixture.hpp"
#include <cuda_runtime.h>
#include <iostream>
using namespace hbflab;
void check(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
__global__ void fill(unsigned* page,unsigned tag){for(unsigned i=threadIdx.x;i<1024;i+=blockDim.x)page[i]=tag+i;}
__global__ void verify(const unsigned* page,unsigned tag,unsigned count,unsigned* errors){
    for(unsigned i=threadIdx.x;i<count;i+=blockDim.x)if(page[i]!=tag+i)atomicAdd(errors,1u);
}
__global__ void row_sum(const float* page,unsigned offset,unsigned count,float* out){
    if(threadIdx.x==0){float sum=0;for(unsigned i=0;i<count;++i)sum+=page[offset+i];*out=sum;}
}
int main(int argc,char** argv) try {
    const std::string test=argc>1?argv[1]:"writeback";
    CapacityOptions options;options.logical_pages=8;options.frames=test=="pins"?2:1;options.backend.geometry={1,1,1,4,2,1};
    CapacityGpu gpu(options);auto& store=gpu.store();require(gpu.device_bytes()==options.frames*4096,"fixed device frame budget");
    cudaStream_t stream=nullptr;check(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
    if(test=="kv" || test=="kv_cross_page") {
        KvOptions config;config.layers=2;config.head_dimension=16;config.element_bytes=4;config.tokens_per_block=2;
        if(test=="kv_cross_page"){config.layers=1;config.head_dimension=1536;config.tokens_per_block=1;}
        const unsigned tokens=test=="kv_cross_page"?2:4;
        KvCache kv(store,config);const auto sequence=kv.create();std::vector<float> key(config.layers*config.head_dimension),value(key.size());
        for(unsigned token=0;token<tokens;++token){std::fill(key.begin(),key.end(),float(token+1));std::fill(value.begin(),value.end(),float(10+token));kv.append(sequence,key.data(),value.data(),key.size()*4);}
        float* result=nullptr;check(cudaMalloc(&result,4));
        for(unsigned token=0;token<tokens;++token)for(unsigned layer=0;layer<config.layers;++layer)for(auto kind:{KvKind::Key,KvKind::Value}) {
            const auto slice=kv.locate(sequence,layer,token,kind);float total=0;
            for(std::size_t at=0;at<slice.bytes;) {
                const auto offset=slice.offset+at;const auto count=(std::min)(slice.bytes-at,std::size_t{4096}-offset%4096);
                const auto lease=gpu.acquire(slice.allocation,offset/4096,PageAccess::Read,stream);
                row_sum<<<1,1,0,stream>>>(static_cast<const float*>(gpu.data(lease)),unsigned(offset%4096/4),unsigned(count/4),result);check(cudaGetLastError());
                gpu.finish(lease);float actual=0;check(cudaMemcpy(&actual,result,4,cudaMemcpyDeviceToHost));total+=actual;at+=count;
            }
            require(total==config.head_dimension*float(token+(kind==KvKind::Key?1:10)),"GPU paged KV row");
        }
        kv.flush(sequence);kv.release(sequence);check(cudaFree(result));require(store.free_pages()==8,"KV GPU lifecycle");
    } else {
        const auto allocation=store.allocate(test=="tail"?4096+7:4*4096);
        if(test=="pins") {
            const auto a=gpu.acquire(allocation,0,PageAccess::Read,stream),b=gpu.acquire(allocation,1,PageAccess::Read,stream);
            rejects([&]{gpu.acquire(allocation,2,PageAccess::Read,stream);});rejects([&]{gpu.writable_data(a);});rejects([&]{gpu.flush();});
            unsigned x=9;rejects([&]{store.write(allocation,0,&x,4);});
            gpu.finish(a);rejects([&]{gpu.finish(a);});const auto c=gpu.acquire(allocation,2,PageAccess::Read,stream);gpu.finish(c);gpu.finish(b);
        } else if(test=="capture") {
            check(cudaStreamBeginCapture(stream,cudaStreamCaptureModeThreadLocal));
            rejects([&]{gpu.acquire(allocation,0,PageAccess::Read,stream);});cudaGraph_t graph=nullptr;check(cudaStreamEndCapture(stream,&graph));check(cudaGraphDestroy(graph));
            require(store.statistics().pins==0,"capture rejection did not pin");
        } else if(test=="tail") {
            const auto lease=gpu.acquire(allocation,1,PageAccess::Write,stream);require(gpu.bytes(lease)==7,"tail bounds");
            check(cudaMemsetAsync(gpu.writable_data(lease),85,7,stream));gpu.finish(lease);gpu.flush();
            std::array<unsigned char,7> actual{};store.read(allocation,4096,actual.data(),actual.size());for(auto v:actual)require(v==85,"tail GPU byte write");
            actual.fill(17);store.write(allocation,4096,actual.data(),actual.size());
            const auto next=gpu.acquire(allocation,1,PageAccess::Read,stream);std::array<unsigned char,7> refreshed{};
            check(cudaMemcpyAsync(refreshed.data(),gpu.data(next),7,cudaMemcpyDeviceToHost,stream));gpu.finish(next);
            require(refreshed==actual,"CPU frame revision refreshes GPU mirror");
            const auto again=gpu.acquire(allocation,1,PageAccess::Read,stream);gpu.finish(again);
            const auto stats=gpu.statistics();require(stats.uploads==2 && stats.downloads==1 && stats.mirror_hits==1 &&
                stats.h2d_bytes==8192 && stats.d2h_bytes==7 && !stats.active_leases,"separate transfer counters and no repeated hit copy");
        } else if(test=="writeback") {
            unsigned* errors=nullptr;check(cudaMalloc(&errors,4));check(cudaMemset(errors,0,4));
            for(unsigned page=0;page<4;++page) {
                const auto lease=gpu.acquire(allocation,page,PageAccess::Write,stream);
                fill<<<1,256,0,stream>>>(static_cast<unsigned*>(gpu.writable_data(lease)),page*10000);check(cudaGetLastError());gpu.finish(lease);
            }
            gpu.flush();
            for(unsigned page=0;page<4;++page) {
                const auto lease=gpu.acquire(allocation,page,PageAccess::Read,stream);
                verify<<<1,256,0,stream>>>(static_cast<const unsigned*>(gpu.data(lease)),page*10000,1024,errors);check(cudaGetLastError());gpu.finish(lease);
            }
            unsigned failures=1;check(cudaMemcpy(&failures,errors,4,cudaMemcpyDeviceToHost));require(!failures,"GPU bytes survived eviction and replay");
            std::array<unsigned,1024> page{};store.read(allocation,4096,page.data(),4096);for(unsigned i=0;i<1024;++i)require(page[i]==10000+i,"GPU write visible to host");
            require(store.statistics().writebacks==4 && store.statistics().resident_frames==1,"writeback budget/count");check(cudaFree(errors));
        } else throw std::invalid_argument("unknown case");
        store.release(allocation);
    }
    check(cudaStreamDestroy(stream));std::cout<<test<<" passed, device bytes="<<gpu.device_bytes()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
