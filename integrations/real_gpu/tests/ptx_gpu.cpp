#include "hbflab/module.hpp"
#include "ptx_fixture.hpp"
#include <cuda.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

using namespace hbflab;
void cuda_ok(cudaError_t e) { if (e!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(e)); }
struct Buffer {
    void* data=nullptr; std::size_t bytes;
    explicit Buffer(std::size_t n):bytes(n) { cuda_ok(cudaMalloc(&data,n)); cuda_ok(cudaMemset(data,0xa5,n)); }
    ~Buffer(){ cudaFree(data); }
};
struct Stream {
    cudaStream_t value=nullptr;
    ~Stream() { if (value) cudaStreamDestroy(value); }
};
int main(int argc,char** argv) try {
    const std::string mode=argc>1?argv[1]:"thread";
    RuntimeOptions options; options.threads=4;
    if (mode=="warp" || mode=="predicate" || mode=="inverted" || mode=="empty" || mode=="vector" || mode=="typed") {
        options.mode=ProducerMode::Warp; options.threads=45;
    }
    if (mode=="timeout") options.timeout_ns=1;
    if (mode=="scaled") { options.time_scale=100000; options.timeout_ns=1000000000; }
    if (mode=="backend_error") options.backend.timing.media_timing.sense=hbf_eventlab::Tick{UINT64_MAX};
    Runtime runtime(options);
    std::vector<unsigned char> input(8192);
    for (unsigned i=0;i<input.size();++i) input[i]=static_cast<unsigned char>(i*19+3);
    const auto* device_input=runtime.upload(input.data(),input.size());
    Buffer output(options.threads*16);
    Stream stream;
    if (mode=="stream") cuda_ok(cudaStreamCreateWithFlags(&stream.value,cudaStreamNonBlocking));
    ModuleInterceptor modules(runtime);
    std::string body="ld.global.u32 %r1, [%rd2]; st.global.u32 [%rd3], %r1;";
    if (mode=="predicate" || mode=="inverted" || mode=="empty") {
        body="and.b32 %r2, %r0, 1; setp.ne.u32 %p0, %r2, 0; mov.u32 %r1, 123; ";
        if (mode=="empty") body+="setp.ne.u32 %p0, %r0, %r0; ";
        // Inactive lanes deliberately use an invalid pointer. Their destination must stay 123.
        const auto pred=mode=="inverted"?"@!%p0":"@%p0";
        const auto false_pred=mode=="inverted"?"@%p0":"@!%p0";
        body+=std::string(false_pred)+" mov.u64 %rd2, 1; "+pred+" ld.global.u32 %r1,[%rd2]; st.global.u32 [%rd3],%r1;";
    }
    if (mode=="vector") body="ld.global.v4.u32 {%r1,%r2,%r3,%r4},[%rd2]; st.global.v4.u32 [%rd3],{%r1,%r2,%r3,%r4};";
    if (mode=="typed") body=
        "ld.global.s8 %r1,[%rd2]; st.global.u8 [%rd3],%r1; "
        "ld.global.u16 %r1,[%rd2+2]; st.global.u16 [%rd3+2],%r1; "
        "ld.global.f32 %f0,[%rd2+4]; st.global.f32 [%rd3+4],%f0; "
        "ld.global.f64 %d0,[%rd2+8]; st.global.f64 [%rd3+8],%d0;";
    if (mode=="alignment") body="add.u64 %rd2,%rd2,1; "+body;
    if (mode=="invalid") body="mov.u64 %rd2,1; "+body;
    if (mode=="output_bounds") body="ld.global.u32 %r1,[%rd2]; st.global.u32 [%rd3+4096],%r1;";
    if (mode=="readonly_write") body="ld.global.u32 %r1,[%rd2]; st.global.u32 [%rd2],%r1;";
    if (mode=="dependent") body="ld.global.u8 %r1,[%rd2]; cvt.u64.u32 %rd5,%r1; add.u64 %rd5,%rd0,%rd5; "
        "ld.global.u8 %r2,[%rd5]; st.global.u8 [%rd3],%r2;";
    if (mode=="signed") body="ld.global.s8 %r1,[%rd2+8]; st.global.s32 [%rd3],%r1;";
    if (mode=="store_predicate") body="ld.global.u32 %r1,[%rd2]; and.b32 %r2,%r0,1; setp.ne.u32 %p0,%r2,0; "
        "@!%p0 mov.u64 %rd3,1; @%p0 st.global.u32 [%rd3],%r1;";
    if (mode=="scaled") body="mov.u64 %rd6,%globaltimer; ld.global.u32 %r1,[%rd2]; mov.u64 %rd7,%globaltimer; "
        "sub.u64 %rd7,%rd7,%rd6; st.global.u64 [%rd3+8],%rd7; st.global.u32 [%rd3],%r1;";
    auto source=ptx_fixture(body);
    if (mode=="multiple") {
        const auto another=ptx_fixture("ld.global.u32 %r1,[%rd2]; add.u32 %r1,%r1,7; st.global.u32 [%rd3],%r1;","plus");
        source+=another.substr(another.find(".visible"));
    }
    auto module=modules.load_ptx(source);
    auto function=modules.function(module,"read");
    if (mode=="multiple") function=modules.function(module,"plus");
    const auto identity=modules.identity(module);
    require(identity.source_sha256==sha256(source) && identity.protocol==2 && identity.frontend==ptx_frontend_version,"identity manifest");
    require(identity.cubin_sha256.size()==64,"loaded binary identity missing");
    if (mode=="coverage") {
        unsigned count_cases=0;
        for (const auto type:{"u8","s8","b8","u16","s16","b16","u32","s32","b32","u64","s64","b64","f32","f64"}) {
            const auto bytes=static_cast<unsigned>(std::stoul(std::string(type).substr(1)))/8;
            for (unsigned count:{1u,2u,4u}) {
                if (bytes*count>16) continue;
                const std::string prefix=type[0]=='f'?(bytes==8?"%d":"%f"):(bytes==8?"%rd":"%r");
                std::string regs=count==1?"":"{";
                for (unsigned i=0;i<count;++i) { if (i) regs+=","; regs+=prefix+std::to_string(type[0]=='f'?i:i+5); }
                if (count>1) regs+="}";
                const auto suffix=(count==1?std::string{}:"v"+std::to_string(count)+".")+type;
                const auto code=ptx_fixture("ld.global."+suffix+" "+regs+",[%rd2]; st.global."+suffix+" [%rd3],"+regs+";");
                const auto matrix_module=modules.load_ptx(code);
                cuda_ok(cudaMemset(output.data,0xa5,output.bytes));
                require(modules.launch(modules.function(matrix_module,"read"),1,options.threads,
                    {&device_input,&output.data},{output.data,output.bytes})==Status::Ok,"matrix launch");
                std::vector<unsigned char> actual(output.bytes);
                cuda_ok(cudaMemcpy(actual.data(),output.data,output.bytes,cudaMemcpyDeviceToHost));
                for (unsigned tid=0;tid<options.threads;++tid) for (unsigned i=0;i<16;++i)
                    require(actual[tid*16+i]==(i<count*bytes?input[tid*16+i]:0xa5),"matrix byte semantics");
                modules.unload(matrix_module); ++count_cases;
            }
        }
        require(runtime.statistics().requests==count_cases*options.threads,"matrix requests bypassed backend");
        std::cout<<"PASS module coverage combinations="<<count_cases<<" requests="<<runtime.statistics().requests<<"\n";
        return 0;
    }
    if (mode=="identity") {
        auto other=modules.load_ptx(source); auto foreign_function=modules.function(other,"read");
        const auto second=modules.identity(other);
        require(second.instance!=identity.instance && second.artifact_sha256==identity.artifact_sha256,"content vs instance identity");
        auto changed=modules.load_ptx(source+"// changed source\n");
        require(modules.identity(changed).artifact_sha256!=identity.artifact_sha256,"raw source identity missing");
        ModuleInterceptor foreign(runtime);
        rejects([&]{foreign.identity(module);});
        rejects([&]{foreign.launch(foreign_function,1,options.threads,{&device_input,&output.data},{output.data,output.bytes});});
        rejects([&]{modules.function(module,"__hbflab_gate");});
        modules.unload(module);
        rejects([&]{modules.identity(module);});
        rejects([&]{modules.launch(function,1,options.threads,{&device_input,&output.data});});
        module=other; function=foreign_function;
    }
    if (mode=="validation") {
        rejects([&]{modules.launch(function,2,options.threads,{&device_input,&output.data});});
        rejects([&]{modules.launch(function,1,options.threads+1,{&device_input,&output.data});});
        rejects([&]{modules.launch(function,1,options.threads,{&device_input});});
        rejects([&]{modules.launch(function,1,options.threads,{nullptr,&output.data});});
        rejects([&]{modules.launch(function,1,options.threads,{&device_input,&output.data},{output.data,output.bytes+1});});
        rejects([&]{modules.load_ptx(ptx_fixture("bra foo; ld.global.u32 %r1,[%rd2];"));});
        // Invalid arity is rejected before JIT; no native fallback.
        rejects([&]{modules.load_ptx(ptx_fixture("add.u32 %r1, %r2; ld.global.u32 %r1,[%rd2];"));});
        bool thread_rejected=false;
        std::thread t([&]{ try { modules.identity(module); } catch (...) { thread_rejected=true; } }); t.join();
        require(thread_rejected,"host-thread ownership not enforced");
        CUcontext saved=nullptr; require(cuCtxPopCurrent(&saved)==CUDA_SUCCESS,"pop context");
        rejects([&]{modules.identity(module);});
        require(cuCtxPushCurrent(saved)==CUDA_SUCCESS,"restore context");
        CUdevice device=0; require(cuCtxGetDevice(&device)==CUDA_SUCCESS,"get device");
        CUcontext other_context=nullptr;
#if CUDA_VERSION >= 13000
        require(cuCtxCreate(&other_context,nullptr,0,device)==CUDA_SUCCESS,"create another context");
#else
        require(cuCtxCreate(&other_context,0,device)==CUDA_SUCCESS,"create another context");
#endif
        rejects([&]{ModuleInterceptor wrong_context(runtime);});
        require(cuCtxDestroy(other_context)==CUDA_SUCCESS,"destroy other context");
        require(cuCtxSetCurrent(saved)==CUDA_SUCCESS,"restore original context");
        require(runtime.statistics().requests==0,"rejected launch touched backend");
        Stream capturing; cuda_ok(cudaStreamCreateWithFlags(&capturing.value,cudaStreamNonBlocking));
        cuda_ok(cudaStreamBeginCapture(capturing.value,cudaStreamCaptureModeThreadLocal));
        rejects([&]{modules.launch(function,1,options.threads,{&device_input,&output.data},{output.data,output.bytes},capturing.value);});
        cudaGraph_t graph=nullptr; cuda_ok(cudaStreamEndCapture(capturing.value,&graph));
        if (graph) cuda_ok(cudaGraphDestroy(graph));
    }
    const bool read_failure=mode=="alignment" || mode=="invalid";
    const bool store_failure=mode=="output_bounds" || mode=="readonly_write";
    const auto span=mode=="readonly_write"?OutputSpan{const_cast<std::uint32_t*>(device_input),input.size()}:OutputSpan{output.data,output.bytes};
    const auto status=modules.launch(function,1,options.threads,{&device_input,&output.data},span,stream.value);
    const auto expected=read_failure || store_failure?Status::InvalidRange:mode=="timeout"?Status::Timeout:mode=="backend_error"?Status::BackendError:Status::Ok;
    require(status==expected,"unexpected served status");
    std::vector<unsigned char> got(output.bytes);
    cuda_ok(cudaMemcpy(got.data(),output.data,got.size(),cudaMemcpyDeviceToHost));
    if (status==Status::Ok) {
        for (unsigned tid=0;tid<options.threads;++tid) {
            unsigned char expected_bytes[16]; std::memset(expected_bytes,0xa5,16);
            if (mode=="predicate" || mode=="inverted" || mode=="empty") {
                bool active=mode=="predicate"?tid%2!=0:mode=="inverted"?tid%2==0:false;
                unsigned value=123; if (active) std::memcpy(&value,input.data()+tid*16,4);
                std::memcpy(expected_bytes,&value,4);
            } else if (mode=="typed") {
                expected_bytes[0]=input[tid*16]; std::memcpy(expected_bytes+2,input.data()+tid*16+2,14);
            } else if (mode=="dependent") expected_bytes[0]=input[input[tid*16]];
            else if (mode=="multiple") {
                std::uint32_t value; std::memcpy(&value,input.data()+tid*16,4); value+=7; std::memcpy(expected_bytes,&value,4);
            }
            else if (mode=="signed") {
                const std::int32_t value=static_cast<std::int8_t>(input[tid*16+8]); std::memcpy(expected_bytes,&value,4);
            } else if (mode=="store_predicate") {
                if (tid%2) std::memcpy(expected_bytes,input.data()+tid*16,4);
            } else if (mode=="scaled") {
                std::memcpy(expected_bytes,input.data()+tid*16,4);
                std::uint64_t elapsed=0; std::memcpy(&elapsed,got.data()+tid*16+8,8);
                bool found=false;
                for (const auto& entry:runtime.last_frontier()) if (entry.producer==tid) {
                    const auto delay=((entry.end_ps-entry.arrival_ps)*options.time_scale+999)/1000;
                    require(elapsed>=delay,"rewritten instruction bypassed target delay"); found=true;
                }
                require(found,"missing modeled timing"); std::memcpy(expected_bytes+8,&elapsed,8);
            }
            else std::memcpy(expected_bytes,input.data()+tid*16,mode=="vector"?16:4);
            require(std::memcmp(expected_bytes,got.data()+tid*16,16)==0,"real GPU result or false-predicate destination mismatch");
        }
        const auto stats=runtime.statistics();
        const unsigned rounds=mode=="typed"?4:mode=="dependent"?2:1;
        const auto parents=mode=="empty"?0:rounds*(options.mode==ProducerMode::Thread?options.threads:(options.threads+31)/32);
        require(stats.requests==parents,"loads bypassed EventLab or coalescing count wrong");
        for (const auto& entry:runtime.last_frontier()) require(entry.ticket==rounds,"round/ticket mismatch");
        require(runtime.drained(),"backend not drained");
        // Same loaded module, same Runtime: independent launch, continuous slot sequence.
        require(modules.launch(function,1,options.threads,{&device_input,&output.data},span,stream.value)==Status::Ok,"repeat launch");
        for (const auto& entry:runtime.last_frontier()) require(entry.ticket==2*rounds,"repeated launch reset tickets");
    } else {
        for (auto b:got) require(b==0xa5,"failed load/store changed result buffer");
        if (read_failure) require(runtime.statistics().requests==0,"invalid frontier reached backend");
        rejects([&]{modules.launch(function,1,options.threads,{&device_input,&output.data},span);});
    }
    std::cout<<"PASS module "<<mode<<" status="<<static_cast<unsigned>(status)<<" requests="<<runtime.statistics().requests
        <<" instance="<<identity.instance<<" artifact="<<identity.artifact_sha256<<"\n";
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
