#include "hbflab/async_module.hpp"
#include "ptx_fixture.hpp"
#include <cuda.h>
#include <fstream>
#include <iterator>
#include <iostream>
#include <array>
using namespace hbflab;
void ck(CUresult c){if(c!=CUDA_SUCCESS)throw std::runtime_error("CUDA "+std::to_string(c));}
int main(int argc,char** argv)try {
    const std::string mode=argc>1?argv[1]:"compare";
    ck(cuInit(0));CUcontext ctx;ck(cuDevicePrimaryCtxRetain(&ctx,0));ck(cuCtxSetCurrent(ctx));
    CUdeviceptr x,w,y,reference;for(auto p:{&x,&w,&y,&reference})ck(cuMemAlloc(p,4096));
    std::array<float,1024> a,b,expected,got;for(unsigned i=0;i<1024;++i){a[i]=(int(i%37)-18)*0.03125f;b[i]=(int(i%71)-35)*0.015625f;}
    ck(cuMemcpyHtoD(x,a.data(),4096));ck(cuMemcpyHtoD(w,b.data(),4096));
    std::ifstream file(HBFLAB_FRAMEWORK_PTX);std::string source((std::istreambuf_iterator<char>(file)),{});require(!source.empty(),"kernel PTX missing");
    CUmodule original;ck(cuModuleLoadData(&original,source.c_str()));CUfunction native;ck(cuModuleGetFunction(&native,original,"m5_weight_mul"));
    if(mode=="precedence")x=w;
    void* args[]={&x,&w,&reference};ck(cuLaunchKernel(native,2,1,1,128,1,1,0,nullptr,args,nullptr));ck(cuCtxSynchronize());ck(cuMemcpyDtoH(expected.data(),reference,4096));
    {
        AsyncRuntimeOptions options;options.audit_limit=4096;LiveTimingService service(options,256,1);service.register_readonly(reinterpret_cast<void*>(w),4096);service.start();
        AsyncModuleInterceptor loader(service);auto m=loader.load_ptx(source);auto f=loader.function(m,"m5_weight_mul");
        rejects([&]{loader.launch(f,{2,128},{&x,&w,&y},{reinterpret_cast<void*>(y),4096},nullptr,{reinterpret_cast<void*>(y),4096});});
        const auto span=mode=="missing"?NativeReadSpan{}:NativeReadSpan{reinterpret_cast<void*>(x),4096};
        loader.launch(f,{2,128},{&x,&w,&y},{reinterpret_cast<void*>(y),4096},nullptr,span);
        if(mode=="missing")rejects([&]{service.synchronize();});
        else {
            service.synchronize();ck(cuMemcpyDtoH(got.data(),y,4096));require(got==expected,"NVCC original/rewritten bytes differ");
            const auto s=service.statistics();require(s.completed==(mode=="precedence"?2048:1024),"modeled/native role counts");
            std::cout<<"framework PTX "<<mode<<" requests="<<s.completed<<" source="<<loader.identity(m).source_sha256<<'\n';loader.unload(m);service.finish();
        }
    }
    ck(cuModuleUnload(original));return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
