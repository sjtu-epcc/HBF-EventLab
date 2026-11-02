#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cuda.h>
#include "hbflab/injection.h"
#include "ptx_fixture.hpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <thread>

// Deliberately links only cuda.lib. All module/launch calls are ordinary Driver API.
void checked(CUresult result) {
    if(result!=CUDA_SUCCESS)throw std::runtime_error("CUDA result "+std::to_string(result));
}
int main(int argc,char** argv) try {
    const std::string test=argc>1?argv[1]:"direct";
    if(test=="exit_code")return 37;
    if(test=="quoted")require(argc==5 && std::string(argv[2])=="two words" &&
        std::string(argv[3])=="quote\"slash\\" && std::string(argv[4]).empty(),"argument quoting");
    const auto dll=GetModuleHandleW(L"hbflab_inject.dll");
    using Query=int(WINAPI*)(HBFLabInjectionStats*,unsigned);
    const auto query=dll?reinterpret_cast<Query>(GetProcAddress(dll,"HBFLabInjectionQuery")):nullptr;
    require(test=="baseline"?!dll:query!=nullptr,"injection presence");
    checked(cuInit(0)); CUcontext ctx=nullptr; checked(cuDevicePrimaryCtxRetain(&ctx,0)); checked(cuCtxSetCurrent(ctx));
    CUdeviceptr input=0,output=0; checked(cuMemAlloc(&input,64)); checked(cuMemAlloc(&output,64));
    std::array<unsigned,16> values{},result{}; for(unsigned i=0;i<16;++i)values[i]=i*19+7;
    checked(cuMemcpyHtoD(input,values.data(),64)); checked(cuMemsetD8(output,0,64));
    auto source=ptx_fixture("ld.global.v4.u32 {%r1,%r2,%r3,%r4}, [%rd2]; st.global.v4.u32 [%rd3], {%r1,%r2,%r3,%r4};");
    auto load=&cuModuleLoadData; auto function=&cuModuleGetFunction; auto launch=&cuLaunchKernel; auto unload=&cuModuleUnload;
    if(test=="exports") {
        const auto driver=GetModuleHandleW(L"nvcuda.dll");
        load=reinterpret_cast<decltype(load)>(GetProcAddress(driver,"cuModuleLoadData"));
        function=reinterpret_cast<decltype(function)>(GetProcAddress(driver,"cuModuleGetFunction"));
        launch=reinterpret_cast<decltype(launch)>(GetProcAddress(driver,"cuLaunchKernel"));
        unload=reinterpret_cast<decltype(unload)>(GetProcAddress(driver,"cuModuleUnload"));
    }
    if(test=="proc" || test=="proc_old" || test=="per_thread") {
        auto resolve=[&](const char* name,int version,void** p) {
            const auto flags=test=="per_thread" && std::string(name)=="cuLaunchKernel"?CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM:CU_GET_PROC_ADDRESS_DEFAULT;
            if(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM)version=7000;
            if(test=="proc_old") {
                using Old=CUresult(CUDAAPI*)(const char*,void**,int,cuuint64_t);
                const auto old=reinterpret_cast<Old>(GetProcAddress(GetModuleHandleW(L"nvcuda.dll"),"cuGetProcAddress"));
                checked(old(name,p,version,flags));
            } else {
                CUdriverProcAddressQueryResult status; checked(cuGetProcAddress(name,p,version,flags,&status));
                require(status==CU_GET_PROC_ADDRESS_SUCCESS,"proc status");
            }
        };
        resolve("cuModuleLoadData",4000,reinterpret_cast<void**>(&load));
        resolve("cuModuleGetFunction",4000,reinterpret_cast<void**>(&function));
        resolve("cuLaunchKernel",4000,reinterpret_cast<void**>(&launch));
        resolve("cuModuleUnload",4000,reinterpret_cast<void**>(&unload));
    }
    CUmodule module=nullptr;
    if(test=="file") {
        const auto path="injection fixture "+std::to_string(GetCurrentProcessId())+".ptx";
        {std::ofstream file(path);file<<source;}
        const auto status=cuModuleLoad(&module,path.c_str()); std::remove(path.c_str());checked(status);
    } else if(test=="load_ex") checked(cuModuleLoadDataEx(&module,source.c_str(),0,nullptr,nullptr));
    else checked(load(&module,source.c_str()));
    CUfunction kernel=nullptr; checked(function(&kernel,module,"read"));
    CUfunction same=nullptr; checked(function(&same,module,"read"));require(same==kernel,"stable function handle");
    CUstream stream=nullptr; if(test=="stream")checked(cuStreamCreate(&stream,CU_STREAM_NON_BLOCKING));
    void* args[]={&input,&output};
    auto run=[&]{checked(launch(kernel,1,1,1,4,1,1,0,stream,args,nullptr));checked(cuStreamSynchronize(stream));
        checked(cuMemcpyDtoH(result.data(),output,64));require(result==values,"GPU result differs");};
    if(test=="reject") {
        require(launch(kernel,2,1,1,4,1,1,0,stream,args,nullptr)==CUDA_ERROR_NOT_SUPPORTED,"shape rejected");
        CUmodule bad=nullptr;const auto branch=ptx_fixture("bra done; done: ld.global.u32 %r1,[%rd2];");
        require(load(&bad,branch.c_str())!=CUDA_SUCCESS && !bad,"invalid PTX rejected");
        require(cuModuleLoadFatBinary(&bad,source.c_str())==CUDA_ERROR_NOT_SUPPORTED,"fatbin rejected");
        CUjit_option option=CU_JIT_MAX_REGISTERS;void* option_value=reinterpret_cast<void*>(32);
        require(cuModuleLoadDataEx(&bad,source.c_str(),1,&option,&option_value)==CUDA_ERROR_NOT_SUPPORTED,"JIT options rejected");
        int attribute=0;require(cuFuncGetAttribute(&attribute,CU_FUNC_ATTRIBUTE_NUM_REGS,kernel)==CUDA_ERROR_NOT_SUPPORTED,"attribute rejected");
        void* unknown=reinterpret_cast<void*>(1);CUdriverProcAddressQueryResult status;
        const auto found=cuGetProcAddress("cuHBFLabMissing",&unknown,13000,0,&status);
        require(found!=CUDA_SUCCESS || (status!=CU_GET_PROC_ADDRESS_SUCCESS && !unknown),"missing proc not fabricated");
    }
    if(test=="thread_owner") {
        CUresult other=CUDA_SUCCESS;
        std::thread worker([&]{checked(cuCtxSetCurrent(ctx));CUfunction f=nullptr;other=function(&f,module,"read");});worker.join();
        require(other==CUDA_ERROR_NOT_SUPPORTED,"foreign host thread rejected");
    }
    if(test=="bounds") {
        const auto saved=input;input+=4;
        require(launch(kernel,1,1,1,4,1,1,0,stream,args,nullptr)==CUDA_ERROR_INVALID_VALUE,"borrow beyond allocation rejected");input=saved;
        void* invalid[]={reinterpret_cast<void*>(1),&output};
        require(launch(kernel,1,1,1,4,1,1,0,stream,invalid,nullptr)==CUDA_ERROR_INVALID_VALUE,"unreadable parameter rejected");
    }
    if(test=="graph") {
        CUgraph graph=nullptr;checked(cuGraphCreate(&graph,0));CUgraphNode node=nullptr;
        CUDA_KERNEL_NODE_PARAMS params{};params.func=kernel;params.gridDimX=params.gridDimY=params.gridDimZ=1;
        params.blockDimX=4;params.blockDimY=params.blockDimZ=1;params.kernelParams=args;
        require(cuGraphAddKernelNode(&node,graph,nullptr,0,&params)==CUDA_ERROR_NOT_SUPPORTED,"graph kernel rejected");
        checked(cuGraphDestroy(graph));
        CUstream capture=nullptr;checked(cuStreamCreate(&capture,CU_STREAM_NON_BLOCKING));
        checked(cuStreamBeginCapture(capture,CU_STREAM_CAPTURE_MODE_THREAD_LOCAL));
        require(launch(kernel,1,1,1,4,1,1,0,capture,args,nullptr)==CUDA_ERROR_NOT_SUPPORTED,"capture rejected");
        checked(cuStreamEndCapture(capture,&graph));checked(cuGraphDestroy(graph));checked(cuStreamDestroy(capture));
    }
    run(); unsigned expected_launches=1;
    if(test=="per_thread_memory") {
        void* p=nullptr;CUdriverProcAddressQueryResult status;
        checked(cuGetProcAddress("cuMemsetD32",&p,7000,CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM,&status));
        require(status==CU_GET_PROC_ADDRESS_SUCCESS,"ptds query");
        const auto set=reinterpret_cast<decltype(&cuMemsetD32)>(p);
        require(set(input,0,16)==CUDA_ERROR_NOT_PERMITTED,"ptds write protected");
        checked(set(output,0,16));
        checked(cuGetProcAddress("cuMemcpyAsync",&p,7000,CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM,&status));
        require(status==CU_GET_PROC_ADDRESS_SUCCESS,"ptsz query");
        const auto cp=reinterpret_cast<decltype(&cuMemcpyAsync)>(p);
        require(cp(input,output,64,nullptr)==CUDA_ERROR_NOT_PERMITTED,"ptsz write protected");
        checked(cp(output,input,64,nullptr));checked(cuStreamSynchronize(CU_STREAM_PER_THREAD));
        checked(cuMemcpyDtoH(result.data(),output,64));require(result==values,"per-thread copy result");
    }
    if(test=="lease") {
        require(cuMemcpyHtoD(input,values.data(),64)==CUDA_ERROR_NOT_PERMITTED,"readonly copy blocked");
        require(cuMemFree(input)==CUDA_ERROR_NOT_PERMITTED,"borrowed free blocked");
        require(cuMemcpyDtoD(input,output,64)==CUDA_ERROR_NOT_PERMITTED,"readonly DtoD blocked");
        require(cuMemcpy(input,output,64)==CUDA_ERROR_NOT_PERMITTED,"readonly generic copy blocked");
        require(cuMemcpyAsync(input,output,64,stream)==CUDA_ERROR_NOT_PERMITTED,"readonly async copy blocked");
        require(cuMemsetD8(input,0,64)==CUDA_ERROR_NOT_PERMITTED,"readonly memset8 blocked");
        require(cuMemsetD16(input,0,32)==CUDA_ERROR_NOT_PERMITTED,"readonly memset16 blocked");
        require(cuMemsetD32(input,0,16)==CUDA_ERROR_NOT_PERMITTED,"readonly memset32 blocked");
        require(cuMemsetD8Async(input,0,64,stream)==CUDA_ERROR_NOT_PERMITTED,"readonly async memset8 blocked");
        require(cuMemsetD16Async(input,0,32,stream)==CUDA_ERROR_NOT_PERMITTED,"readonly async memset16 blocked");
        require(cuMemsetD32Async(input,0,16,stream)==CUDA_ERROR_NOT_PERMITTED,"readonly async memset32 blocked");
        require(cuDevicePrimaryCtxRelease(0)==CUDA_ERROR_NOT_PERMITTED,"primary release blocked");
        require(cuDevicePrimaryCtxReset(0)==CUDA_ERROR_NOT_PERMITTED,"primary reset blocked");
        const auto saved=input;input=output;
        require(launch(kernel,1,1,1,4,1,1,0,stream,args,nullptr)==CUDA_ERROR_INVALID_VALUE,"changed input blocked");input=saved;
        run();++expected_launches;
    }
    if(test=="multiple") {
        CUmodule second=nullptr;checked(load(&second,source.c_str()));require(second!=module,"module identity");
        const auto stale=kernel;checked(function(&kernel,second,"read"));require(kernel!=stale,"function identity");
        checked(unload(module));require(launch(stale,1,1,1,4,1,1,0,stream,args,nullptr)==CUDA_ERROR_INVALID_HANDLE,"stale function rejected");
        module=second;run();++expected_launches;
    }
    if(test=="reload") {
        const auto old=module;checked(unload(module));values[0]+=100;checked(cuMemcpyHtoD(input,values.data(),64));
        checked(load(&module,source.c_str()));require(old!=module,"module token never reused");checked(function(&kernel,module,"read"));
        run();++expected_launches;
    }
    if(query) {
        HBFLabInjectionStats stats{}; require(query(&stats,sizeof(stats)) && stats.active && stats.version==1,"diagnostics");
        const auto per_launch=test=="warp"?1u:4u;
        require(stats.launches==expected_launches && stats.requests==per_launch*expected_launches && stats.live_modules==1,"EventLab requests missing");
        std::cout<<"injected launches="<<stats.launches<<" requests="<<stats.requests<<"\n";
    }
    if(test=="exit_live")return 0;
    checked(unload(module));
    if(query) {
        require(unload(module)==CUDA_ERROR_INVALID_HANDLE,"stale module rejected");
        HBFLabInjectionStats stats{};require(query(&stats,sizeof(stats)) && !stats.live_modules,"module cleanup");
    }
    // Borrowing must never free or replace the application's allocation.
    checked(cuMemcpyHtoD(input,values.data(),64));checked(cuMemFree(input));checked(cuMemFree(output));
    if(stream)checked(cuStreamDestroy(stream));checked(cuDevicePrimaryCtxRelease(0));
    std::cout<<test<<" passed\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
