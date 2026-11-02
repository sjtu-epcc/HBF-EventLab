#include "hbflab/module.hpp"
#include "ptx_fixture.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hbflab;
void check(cudaError_t e) { if (e!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(e)); }
struct Output {
    void* data=nullptr;
    Output(){check(cudaMalloc(&data,64));}
    ~Output(){cudaFree(data);}
};
struct Stream {
    cudaStream_t value=nullptr;
    Stream(){check(cudaStreamCreateWithFlags(&value,cudaStreamNonBlocking));}
    ~Stream(){cudaStreamDestroy(value);}
};
// Deliberately hold the default stream without making CUDA calls in its callback.
// The watchdog bounds failures; correctness is asserted by which side released it.
struct DefaultBlock {
    std::atomic<bool> started{false},release{false},forced{false};
    std::thread watchdog;
    static void CUDART_CB callback(void* p) {
        auto& self=*static_cast<DefaultBlock*>(p); self.started=true;
        while (!self.release.load()) std::this_thread::yield();
    }
    DefaultBlock() {
        watchdog=std::thread([this]{
            for (unsigned i=0;i<200 && !release.load();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!release.load()) { forced=true; release=true; }
        });
        try { check(cudaLaunchHostFunc(nullptr,callback,this)); }
        catch (...) { release=true; watchdog.join(); throw; }
        while (!started.load() && !forced.load()) std::this_thread::yield();
    }
    ~DefaultBlock(){release=true; watchdog.join(); cudaStreamSynchronize(nullptr);}
};
int main(int argc,char** argv) try {
    const std::string mode=argc>1?argv[1]:"hit";
    RuntimeOptions ro; ro.threads=4;
    Runtime runtime(ro);
    std::uint32_t input[16]{}; for (unsigned i=0;i<16;++i) input[i]=100+i;
    const auto* gpu_input=runtime.upload(input,sizeof(input));
    Output output; Stream stream;
    ModuleOptions options;
    if (mode=="lru") options.cache_entries=2;
    if (mode=="disabled") options.cache_entries=0;
    if (mode=="bytes") options.cache_bytes=1;
    ModuleInterceptor loader(runtime,options);
    const auto source=ptx_fixture();
    auto a=loader.load_ptx(source); auto fa=loader.function(a,"read");
    require(loader.function(a,"read").value==fa.value && loader.statistics().live_functions==1,"function lookup leaks handles");
    auto run=[&](FunctionHandle f,Output& out) {
        require(loader.launch(f,1,4,{&gpu_input,&out.data},{out.data,64},stream.value)==Status::Ok,"cached module launch");
    };
    if (mode=="hit" || mode=="bindings" || mode=="clear" || mode=="disabled" || mode=="bytes") {
        auto b=loader.load_ptx(source); auto fb=loader.function(b,"read");
        const auto ia=loader.identity(a),ib=loader.identity(b);
        require(ia.instance!=ib.instance && ia.artifact_sha256==ib.artifact_sha256 && ia.cubin_sha256==ib.cubin_sha256,"cached identity changed");
        const bool cached=mode!="disabled" && mode!="bytes";
        require(loader.statistics().jit_compilations==(cached?1:2) && loader.statistics().cache_hits==(cached?1:0),"JIT cache accounting");
        if (!cached) require(loader.statistics().cache_bytes==0 && loader.statistics().cache_entries==0,"disabled/oversized cache retained data");
        if (mode=="clear") {
            loader.clear_cache(); require(loader.statistics().cache_bytes==0,"clear did not release cache");
            loader.unload(loader.load_ptx(source)); require(loader.statistics().jit_compilations==2,"clear reused stale cache");
        }
        Output second;
        run(fa,output); run(fb,second);
        loader.unload(a);
        rejects([&]{run(fa,output);});
        run(fb,output); // Unloading a sibling must not invalidate a cached instance.
        if (mode=="bindings") {
            std::uint32_t actual[16]{}; check(cudaMemcpy(actual,second.data,64,cudaMemcpyDeviceToHost));
            for (unsigned i=0;i<4;++i) require(actual[i*4]==input[i*4],"second binding bytes");
        }
        loader.unload(b);
        require(loader.statistics().live_modules==0 && loader.statistics().live_functions==0,"unload left handles");
    } else if (mode=="lru") {
        auto b=loader.load_ptx(source+"//B");
        loader.unload(loader.load_ptx(source)); // Touch A, so B is least recently used.
        auto c=loader.load_ptx(source+"//C");
        require(loader.statistics().cache_evictions==1 && loader.statistics().cache_entries==2,"LRU capacity");
        loader.unload(loader.load_ptx(source)); require(loader.statistics().jit_compilations==3,"recent A was evicted");
        loader.unload(loader.load_ptx(source+"//B")); require(loader.statistics().jit_compilations==4,"old B was not evicted");
        run(loader.function(b,"read"),output); run(fa,output); // Eviction does not unload live modules.
        loader.unload(b); loader.unload(c); loader.unload(a);
    } else if (mode=="byte_lru") {
        const auto budget=loader.statistics().cache_bytes;
        require(budget>0,"missing compiled binary size");
        ModuleInterceptor bounded(runtime,{8,budget});
        bounded.load_ptx(source); bounded.load_ptx(source+"//B");
        const auto stats=bounded.statistics();
        require(stats.cache_bytes<=budget && stats.cache_entries==1 && stats.cache_evictions==1,"binary byte budget not enforced");
        run(fa,output);
    } else if (mode=="failure") {
        const auto invalid=ptx_fixture("add.u32 %rd5,%rd6,%rd7; ld.global.u32 %r1,[%rd2];");
        const auto before=loader.statistics();
        rejects([&]{loader.load_ptx(invalid);}); rejects([&]{loader.load_ptx(invalid);});
        require(loader.statistics().jit_compilations==before.jit_compilations+2 && loader.statistics().cache_entries==before.cache_entries,"failed JIT cached");
        require(loader.statistics().live_modules==1,"failed load leaked a module"); run(fa,output);
    } else if (mode=="isolation") {
        ModuleInterceptor other(runtime);
        auto b=other.load_ptx(source);
        require(other.statistics().jit_compilations==1 && other.statistics().cache_hits==0,"cache escaped interceptor context");
        rejects([&]{other.function(a,"read");});
        loader.clear_cache(); run(fa,output);
        require(other.launch(other.function(b,"read"),1,4,{&gpu_input,&output.data},{output.data,64},stream.value)==Status::Ok,"independent cache instance");
    } else if (mode=="stream_order") {
        run(fa,output); // Resolve lazy loading before holding an unrelated stream.
        {
            DefaultBlock block;
            run(fa,output);
            require(!block.forced.load(),"binding still depends on the blocked default stream");
        }
    } else throw std::invalid_argument("unknown cache test");
    std::uint32_t actual[16]{}; check(cudaMemcpy(actual,output.data,64,cudaMemcpyDeviceToHost));
    for (unsigned i=0;i<4;++i) require(actual[i*4]==input[i*4],"cached module real bytes");
    const auto stats=loader.statistics();
    std::cout<<"PASS cache "<<mode<<" jit="<<stats.jit_compilations<<" hits="<<stats.cache_hits
        <<" evictions="<<stats.cache_evictions<<" bytes="<<stats.cache_bytes<<"\n";
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
