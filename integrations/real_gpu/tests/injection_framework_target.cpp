#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cuda.h>
#include "hbflab/injection.h"
#include "ptx_fixture.hpp"
#include <fstream>
#include <iterator>
#include <array>
#include <iostream>
void ck(CUresult c){if(c!=CUDA_SUCCESS)throw std::runtime_error("CUDA "+std::to_string(c));}
int main()try {
    ck(cuInit(0));CUcontext ctx;ck(cuDevicePrimaryCtxRetain(&ctx,0));ck(cuCtxSetCurrent(ctx));
    CUdeviceptr x,w,y[2];for(auto p:{&x,&w,&y[0],&y[1]})ck(cuMemAlloc(p,4096));
    std::array<float,1024> a,b,got;for(unsigned i=0;i<1024;++i){a[i]=(int(i%31)-15)*0.03125f;b[i]=(int(i%71)-35)*0.015625f;}
    ck(cuMemcpyHtoD(x,a.data(),4096));ck(cuMemcpyHtoD(w,b.data(),4096));
    std::ifstream file(HBFLAB_FRAMEWORK_PTX);std::string ptx((std::istreambuf_iterator<char>(file)),{});
    CUmodule m;ck(cuModuleLoadData(&m,ptx.c_str()));CUfunction f;ck(cuModuleGetFunction(&f,m,"m5_weight_mul"));
    CUstream streams[2];for(auto& s:streams)ck(cuStreamCreate(&s,CU_STREAM_NON_BLOCKING));
    for(unsigned j=0;j<2;++j){void* args[]={&x,&w,&y[j]};ck(cuLaunchKernel(f,2,1,1,128,1,1,0,streams[j],args,nullptr));}
    ck(cuCtxSynchronize());
    for(auto p:y){ck(cuMemcpyDtoH(got.data(),p,4096));for(unsigned i=0;i<1024;++i)require(got[i]==a[i]*b[i],"framework injection output mismatch");}
    using Query=int(WINAPI*)(HBFLabInjectionAsyncStats*,unsigned);const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"hbflab_inject.dll"),"HBFLabInjectionAsyncQuery"));
    HBFLabInjectionAsyncStats s{};require(query && query(&s,sizeof(s)) && s.requests_completed==2048 && s.launches_completed==2 && s.peak_active_launches==2,"framework injection ownership/accounting");
    ck(cuModuleUnload(m));for(auto p:{x,w,y[0],y[1]})ck(cuMemFree(p));for(auto st:streams)ck(cuStreamDestroy(st));ck(cuDevicePrimaryCtxRelease(0));
    std::cout<<"framework injected requests="<<s.requests_completed<<" peak="<<s.peak_active_launches<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
