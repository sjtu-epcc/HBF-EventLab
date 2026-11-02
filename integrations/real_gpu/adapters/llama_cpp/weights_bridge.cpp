#include "weights_bridge.hpp"
#include "hbflab/async_module.hpp"
#include "m5_kernel_embedded.hpp"
#include <cuda.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
using namespace hbflab;
using json=nlohmann::ordered_json;
namespace {
void ck(CUresult c){if(c!=CUDA_SUCCESS)throw std::runtime_error("M5 Driver error "+std::to_string(c));}
struct State {
    LiveTimingService* service=nullptr;std::unique_ptr<AsyncModuleInterceptor> loader;
    AsyncModuleHandle module{};AsyncFunctionHandle function{};CUmodule raw=nullptr;CUfunction raw_function=nullptr;
    std::filesystem::path output;std::ofstream calls;unsigned step=0;std::uint64_t step_count=0,total=0,rows=0;int mode=0;
} state;
void save(const char* name,const json& j){std::ofstream f(state.output/name);f<<j.dump(2)<<'\n';f.close();if(!f)throw std::runtime_error("M5 output failure");}
}
void m5_begin(LiveTimingService* service,int mode,const char* output){
    auto& s=state;s.service=service;s.mode=mode;s.output=output;s.calls.open(s.output/"ptx-calls.jsonl");if(!s.calls)throw std::runtime_error("M5 calls log");
    std::ofstream(s.output/"weight_mul.ptx",std::ios::binary)<<embedded_m5_kernel;
    if(service){s.loader=std::make_unique<AsyncModuleInterceptor>(*service);s.module=s.loader->load_ptx(embedded_m5_kernel);s.function=s.loader->function(s.module,"m5_weight_mul");
        const auto i=s.loader->identity(s.module);save("ptx-module.json",{{"frontend",i.frontend},{"protocol",i.protocol},{"source_sha256",i.source_sha256},
            {"rewritten_sha256",i.rewritten_sha256},{"helper_sha256",i.helper_sha256},{"artifact_sha256",i.artifact_sha256},{"cubin_sha256",i.cubin_sha256},
            {"instance",i.instance},{"context",i.context_id},{"target_sm",i.target_sm},{"driver",i.driver_version},{"roles",{{"0","native activation 4096B"},{"1","registered HBF weight 4096B"},{"2","output 4096B"}}}});
        std::ofstream(s.output/"weight_mul.rewritten.ptx",std::ios::binary)<<rewrite_async_ptx(embedded_m5_kernel).text;
    }else{ck(cuModuleLoadData(&s.raw,embedded_m5_kernel.c_str()));ck(cuModuleGetFunction(&s.raw_function,s.raw,"m5_weight_mul"));save("ptx-module.json",{{"modeled",false},{"source_sha256",sha256(embedded_m5_kernel)}});}
}
void m5_step(){state.step_count=0;++state.step;}
void m5_mul(const float* input,const float* weight,float* output,unsigned rows,cudaStream_t stream){
    auto& s=state;
    for(unsigned row=0;row<rows;++row){const auto* x=input+row*1024;auto* y=output+row*1024;std::vector<void*> args{&x,&weight,&y};std::uint64_t launch=0;
        if(s.loader){launch=s.loader->launch(s.function,{2,128},args,{y,4096},stream,{x,4096});s.step_count+=1024;s.total+=1024;}
        else ck(cuLaunchKernel(s.raw_function,2,1,1,128,1,1,0,reinterpret_cast<CUstream>(stream),args.data(),nullptr));
        s.calls<<json({{"step",s.step-1},{"row",row},{"launch",launch},{"weight",reinterpret_cast<std::uintptr_t>(weight)},{"activation",reinterpret_cast<std::uintptr_t>(x)},
            {"output",reinterpret_cast<std::uintptr_t>(y)},{"bytes",4096},{"modeled_requests",s.loader?1024:0}}).dump()<<'\n';++s.rows;
    }if(!s.calls)throw std::runtime_error("M5 calls write");
}
std::uint64_t m5_step_requests(){return state.step_count;}
std::uint64_t m5_total_requests(){return state.total;}
void m5_finish(){auto& s=state;if(s.loader){s.loader->unload(s.module);s.loader.reset();}if(s.raw){ck(cuCtxSynchronize());ck(cuModuleUnload(s.raw));s.raw=nullptr;}
    s.calls.close();if(!s.calls)throw std::runtime_error("M5 calls close");save("ptx-summary.json",{{"status","pass"},{"mode",s.mode},{"rows",s.rows},{"modeled_requests",s.total},
        {"source","pinned llama.cpp k_bin_bcast F32 multiply; contiguous single-row specialization"},{"scope","selected blk.10.ffn_norm.weight; activation explicitly native; no HBF calls in kernel CUDA source"}});}
void m5_abort(){state.loader.reset();if(state.raw){cuModuleUnload(state.raw);state.raw=nullptr;}}
