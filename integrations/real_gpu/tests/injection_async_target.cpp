#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cuda.h>
#include "hbflab/injection.h"
#include "ptx_fixture.hpp"
#include <iostream>
#include <array>
void ck(CUresult c){if(c!=CUDA_SUCCESS)throw std::runtime_error("CUDA "+std::to_string(c));}
int main(int argc,char** argv)try {
    const std::string mode=argc>1?argv[1]:"concurrent";
    ck(cuInit(0));CUcontext ctx;ck(cuDevicePrimaryCtxRetain(&ctx,0));ck(cuCtxSetCurrent(ctx));
    CUdeviceptr input,out[2];ck(cuMemAlloc(&input,4096));for(auto& p:out)ck(cuMemAlloc(&p,4096));
    std::array<unsigned,1024> expected{},got{};for(unsigned i=0;i<1024;++i)expected[i]=i+17;
    ck(cuMemcpyHtoD(input,expected.data(),4096));for(auto p:out)ck(cuMemsetD8(p,0,4096));
    auto source=ptx_fixture("mov.u32 %r2,0; Loop: ld.global.u32 %r1,[%rd2]; add.u32 %r2,%r2,1; setp.lt.u32 %p0,%r2,32; @%p0 bra Loop; st.global.u32 [%rd3],%r1;");
    replace_one(source,"mov.u32 %r0, %tid.x;","mov.u32 %r0,%tid.x; mov.u32 %r4,%ctaid.x; mad.lo.u32 %r0,%r4,32,%r0;");
    const bool failure=mode=="fault" || mode=="fault_event" || mode=="fault_query";
    if(failure)replace_one(source,"[%rd2]","[%rd2+8192]");
    CUmodule module;ck(cuModuleLoadData(&module,source.c_str()));CUfunction f;ck(cuModuleGetFunction(&f,module,"read"));
    CUstream streams[2];for(auto& s:streams)ck(cuStreamCreate(&s,CU_STREAM_NON_BLOCKING));
    auto launch=&cuLaunchKernel;
    if(mode=="proc"){CUdriverProcAddressQueryResult status;ck(cuGetProcAddress("cuLaunchKernel",reinterpret_cast<void**>(&launch),7000,CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM,&status));require(status==CU_GET_PROC_ADDRESS_SUCCESS,"lookup");}
    const unsigned loops=mode=="reuse"?8:failure?1:2;
    for(unsigned j=0;j<loops;++j){void* args[]={&input,&out[j%2]};ck(launch(f,2,1,1,32,1,1,0,streams[j%2],args,nullptr));}
    if(mode=="fault_event"){CUevent e;ck(cuEventCreate(&e,CU_EVENT_DISABLE_TIMING));ck(cuEventRecord(e,streams[0]));require(cuEventSynchronize(e)!=CUDA_SUCCESS,"event lost terminal failure");return 0;}
    if(mode=="fault_query"){CUresult r;do{r=cuStreamQuery(streams[0]);}while(r==CUDA_ERROR_NOT_READY);require(r!=CUDA_SUCCESS,"query lost terminal failure");return 0;}
    if(failure){require(cuCtxSynchronize()!=CUDA_SUCCESS,"asynchronous failure was lost");std::cout<<"failure surfaced at synchronize\n";return 0;}
    if(mode=="events"){CUevent e;ck(cuEventCreate(&e,CU_EVENT_DISABLE_TIMING));ck(cuEventRecord(e,streams[1]));ck(cuEventSynchronize(e));ck(cuEventQuery(e));ck(cuStreamQuery(streams[1]));ck(cuEventDestroy(e));}
    ck(cuStreamSynchronize(streams[0]));ck(cuCtxSynchronize());
    for(auto p:out){ck(cuMemcpyDtoH(got.data(),p,4096));for(unsigned i=0;i<64;++i)require(got[4*i]==expected[4*i],"actual bytes");}
    require(cuMemFree(input)==CUDA_ERROR_NOT_PERMITTED,"input lease missing");
    using Query=int(WINAPI*)(HBFLabInjectionAsyncStats*,unsigned);
    const auto query=reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"hbflab_inject.dll"),"HBFLabInjectionAsyncQuery"));
    HBFLabInjectionAsyncStats stats{};require(query && query(&stats,sizeof(stats)),"async diagnostics");
    require(stats.launches_completed==loops && stats.requests_completed==loops*64*32 && stats.peak_active_launches==2,"async concurrency/accounting");
    ck(cuModuleUnload(module));void* args[]={&input,&out[0]};require(launch(f,2,1,1,32,1,1,0,streams[0],args,nullptr)==CUDA_ERROR_INVALID_HANDLE,"stale function");
    ck(cuMemFree(input));for(auto p:out)ck(cuMemFree(p));for(auto s:streams)ck(cuStreamDestroy(s));ck(cuDevicePrimaryCtxRelease(0));
    std::cout<<"async injection "<<mode<<" requests="<<stats.requests_completed<<" peak="<<stats.peak_active_launches<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
