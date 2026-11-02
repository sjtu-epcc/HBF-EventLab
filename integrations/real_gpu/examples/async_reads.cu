#include "hbflab/async_runtime.hpp"
#include "hbflab/async_device.cuh"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace hbflab;
__global__ void independent_reads(AsyncDeviceView view,const unsigned* input,unsigned* output) {
    const auto i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i%3==0)return;
    AsyncDeviceClient client(view);
    unsigned sum=0;
    for(unsigned n=0;n<i%5+1;++n) {
        unsigned value=0;
        if(client.load(input+i,value)!=Status::Ok)return;
        sum+=value;
    }
    output[i]=sum;
}
static void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
int main(){try{
    constexpr unsigned count=64;
    AsyncRuntime runtime;
    std::vector<unsigned> input(count);for(unsigned i=0;i<count;++i)input[i]=i+1;
    const auto* device=static_cast<const unsigned*>(runtime.upload(input.data(),input.size()*sizeof(unsigned)));
    unsigned* output=nullptr;ck(cudaMalloc(reinterpret_cast<void**>(&output),count*2*sizeof(unsigned)));
    ck(cudaMemset(output,0,count*2*sizeof(unsigned)));
    cudaStream_t streams[2]{};for(auto& stream:streams)ck(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
    runtime.prepare({{4,16},{4,16}});
    for(unsigned k=0;k<2;++k) {
        auto* result=output+k*count;
        runtime.launch(k,reinterpret_cast<const void*>(independent_reads),{&device,&result},streams[k]);
    }
    if(runtime.serve()!=Status::Ok)throw std::runtime_error("asynchronous service failed");
    std::vector<unsigned> actual(count*2);ck(cudaMemcpy(actual.data(),output,actual.size()*sizeof(unsigned),cudaMemcpyDeviceToHost));
    for(unsigned i=0;i<actual.size();++i) {
        const auto local=i%count,expected=local%3==0?0:input[local]*(local%5+1);
        if(actual[i]!=expected)throw std::runtime_error("wrong GPU result");
    }
    const auto stats=runtime.statistics();
    std::cout<<"completed="<<stats.completed<<" peak_inflight="<<stats.peak_inflight
             <<" active_launches="<<stats.peak_active_launches<<" drained="<<runtime.drained()<<'\n';
    ck(cudaFree(output));for(auto stream:streams)ck(cudaStreamDestroy(stream));return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
