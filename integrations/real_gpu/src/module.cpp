#include "hbflab/module.hpp"
#include "hbflab/ptx_binding.hpp"
#include "ptx_helper_embedded.hpp"
#include <cuda.h>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <list>
#include <map>
#include <stdexcept>
#include <thread>

namespace hbflab {
namespace {
void check(CUresult code, const char* stage, const char* log = "") {
    if (code==CUDA_SUCCESS) return;
    const char* name=nullptr; cuGetErrorName(code,&name);
    throw std::runtime_error(std::string(stage)+": "+(name?name:"CUDA driver error")+" "+log);
}
std::uint64_t next_id() {
    static std::atomic<std::uint64_t> next{1};
    // Process-local monotonic capability IDs prevent CUDA handle-address reuse/ABA.
    auto value=next.load();
    do { if (value==UINT64_MAX) throw std::overflow_error("module identity exhausted"); }
    while (!next.compare_exchange_weak(value,value+1));
    return value;
}
struct Link {
    CUlinkState value=nullptr;
    ~Link() { if (value) cuLinkDestroy(value); }
};
}
struct ModuleInterceptor::Impl {
    struct Module {
        CUmodule driver=nullptr; CUdeviceptr binding=0; ModuleIdentity identity;
        std::map<std::string,std::uint64_t> handles;
    };
    struct Function { std::uint64_t module; CUfunction driver; unsigned parameters; };
    struct Cached { std::string key, sha256; std::vector<char> binary; };
    Runtime& runtime; DeviceView view;
    CUcontext context=nullptr; CUdevice device=0; int sm=0, driver=0;
    unsigned long long context_id=0;
    const std::thread::id owner=std::this_thread::get_id();
    std::map<std::uint64_t,Module> modules;
    std::map<std::uint64_t,Function> functions;
    ModuleOptions options;
    ModuleStats stats;
    std::list<Cached> cache; // MRU at front; binaries only, never module bindings.
    PtxBinding* staging=nullptr;
    const std::string helper_hash=sha256(embedded_ptx_helper);
    bool poisoned=false;
    explicit Impl(Runtime& r,ModuleOptions o):runtime(r),view(r.view()),options(o) {
        check(cuCtxGetCurrent(&context),"current context");
        if (!context) throw std::runtime_error("Runtime must initialize a CUDA context");
        check(cuCtxGetId(context,&context_id),"context identity");
        CUcontext range_context=nullptr;
        check(cuPointerGetAttribute(&range_context,CU_POINTER_ATTRIBUTE_CONTEXT,
            reinterpret_cast<CUdeviceptr>(view.ranges)),"Runtime allocation context");
        if (range_context!=context) throw std::logic_error("Runtime and module must share the same CUDA context");
        check(cuCtxGetDevice(&device),"current device");
        int major=0,minor=0;
        check(cuDeviceGetAttribute(&major,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,device),"device architecture");
        check(cuDeviceGetAttribute(&minor,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,device),"device architecture");
        sm=major*10+minor; check(cuDriverGetVersion(&driver),"driver version");
        if (sm<75) throw std::runtime_error("PTX helper requires SM75+");
        check(cuMemAllocHost(reinterpret_cast<void**>(&staging),sizeof(PtxBinding)),"allocate binding staging");
        new (staging) PtxBinding{};
    }
    ~Impl() {
        // Context may have changed since the last operation. Restore it after cleanup.
        if (cuCtxPushCurrent(context)!=CUDA_SUCCESS) return;
        cuCtxSynchronize();
        for (const auto& m:modules) cuModuleUnload(m.second.driver);
        if (staging) cuMemFreeHost(staging);
        CUcontext ignored=nullptr; cuCtxPopCurrent(&ignored);
    }
    void current() const {
        if (owner!=std::this_thread::get_id()) throw std::logic_error("module interceptor used from another host thread");
        CUcontext actual=nullptr; check(cuCtxGetCurrent(&actual),"current context");
        if (actual!=context) throw std::logic_error("module interceptor used in a different CUDA context");
    }
    Module& module(ModuleHandle handle) {
        const auto it=modules.find(handle.value);
        if (it==modules.end()) throw std::invalid_argument("foreign, stale or unknown module handle");
        return it->second;
    }
    void cache_binary(const std::string& key,const std::string& hash,std::vector<char>& binary) {
        if (!options.cache_entries || !options.cache_bytes || binary.size()>options.cache_bytes) return;
        // Allocate the entry before eviction so allocation failure does not discard valid entries.
        cache.push_front({key,hash,std::move(binary)});
        stats.cache_bytes+=cache.front().binary.size();
        while (cache.size()>options.cache_entries || stats.cache_bytes>options.cache_bytes) {
            stats.cache_bytes-=cache.back().binary.size(); cache.pop_back(); ++stats.cache_evictions;
        }
    }
};
ModuleInterceptor::ModuleInterceptor(Runtime& r,ModuleOptions o):impl_(std::make_unique<Impl>(r,o)) {}
ModuleInterceptor::~ModuleInterceptor()=default;
ModuleHandle ModuleInterceptor::load_ptx(std::string_view source) {
    auto& s=*impl_; s.current();
    if (s.poisoned) throw std::logic_error("interceptor is terminal after a failed launch");
    s.runtime.view();
    const auto rewritten=rewrite_ptx(source); // Entire module validated before any JIT/load.
    if (rewritten.target_sm>static_cast<unsigned>(s.sm)) throw std::invalid_argument("PTX target exceeds current GPU architecture");
    ModuleIdentity identity;
    identity.source_sha256=sha256(source); identity.rewritten_sha256=sha256(rewritten.text);
    identity.helper_sha256=s.helper_hash; identity.entries=rewritten.entries;
    identity.device=s.device; identity.target_sm=s.sm; identity.driver_version=s.driver;
    identity.context_id=s.context_id;
    identity.artifact_sha256=sha256("HBFLab-PTX\n"+std::to_string(ptx_frontend_version)+"\n"+std::to_string(abi_version)+"\n"+
        identity.source_sha256+"\n"+identity.rewritten_sha256+"\n"+identity.helper_sha256+"\n"+
        std::to_string(s.sm)+"\n"+std::to_string(s.driver));
    identity.instance=next_id();
    const auto hit=std::find_if(s.cache.begin(),s.cache.end(),[&](const Impl::Cached& c){return c.key==identity.artifact_sha256;});
    std::vector<char> compiled;
    const void* image=nullptr;
    if (hit!=s.cache.end()) {
        s.cache.splice(s.cache.begin(),s.cache,hit); ++s.stats.cache_hits;
        image=s.cache.front().binary.data(); identity.cubin_sha256=s.cache.front().sha256;
    } else {
        ++s.stats.jit_compilations;
        char log[8192]{};
        CUjit_option options[]={CU_JIT_ERROR_LOG_BUFFER,CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES};
        void* values[]={log,reinterpret_cast<void*>(sizeof(log))};
        Link link;
        check(cuLinkCreate(2,options,values,&link.value),"create PTX link",log);
        check(cuLinkAddData(link.value,CU_JIT_INPUT_PTX,const_cast<char*>(embedded_ptx_helper.c_str()),
            embedded_ptx_helper.size()+1,"hbflab-helper",0,nullptr,nullptr),"link helper",log);
        check(cuLinkAddData(link.value,CU_JIT_INPUT_PTX,const_cast<char*>(rewritten.text.c_str()),
            rewritten.text.size()+1,"rewritten-input",0,nullptr,nullptr),"link rewritten PTX",log);
        void* cubin=nullptr; std::size_t size=0;
        check(cuLinkComplete(link.value,&cubin,&size),"complete PTX link",log);
        const auto* begin=static_cast<const char*>(cubin);
        compiled.assign(begin,begin+size); // Link owns cubin; copy before destroying it.
        identity.cubin_sha256=sha256(std::string_view(compiled.data(),compiled.size()));
        image=compiled.data();
    }
    Impl::Module module; module.identity=identity;
    check(cuModuleLoadData(&module.driver,image),"load linked module");
    try {
        std::size_t binding_bytes=0;
        check(cuModuleGetGlobal(&module.binding,&binding_bytes,module.driver,"__hbflab_binding"),"locate helper binding");
        if (binding_bytes!=sizeof(PtxBinding)) throw std::runtime_error("helper binding ABI mismatch");
        // A module has no runnable binding until launch; its handles never escape.
        if (!compiled.empty()) s.cache_binary(identity.artifact_sha256,identity.cubin_sha256,compiled);
        s.modules.emplace(identity.instance,std::move(module));
    } catch (...) { cuModuleUnload(module.driver); throw; }
    return {identity.instance};
}
ModuleHandle ModuleInterceptor::load_file(const std::filesystem::path& path) {
    impl_->current();
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) throw std::invalid_argument("cannot open PTX file");
    const auto bytes=file.tellg();
    if (bytes<=0 || bytes>1024*1024) throw std::invalid_argument("PTX file size outside limit");
    std::string text(static_cast<std::size_t>(bytes),'\0'); file.seekg(0);
    if (!file.read(text.data(),static_cast<std::streamsize>(text.size()))) throw std::runtime_error("cannot read complete PTX file");
    return load_ptx(text);
}
FunctionHandle ModuleInterceptor::function(ModuleHandle handle,const std::string& name) {
    auto& s=*impl_; s.current(); auto& m=s.module(handle);
    const auto existing=m.handles.find(name);
    if (existing!=m.handles.end()) return {existing->second};
    for (const auto& entry:m.identity.entries) if (entry.name==name) {
        CUfunction f=nullptr; check(cuModuleGetFunction(&f,m.driver,name.c_str()),"get rewritten entry");
        const auto id=next_id(); s.functions.emplace(id,Impl::Function{handle.value,f,static_cast<unsigned>(entry.parameter_bytes.size())});
        try { m.handles.emplace(name,id); } catch (...) { s.functions.erase(id); throw; }
        return {id};
    }
    throw std::invalid_argument("entry absent from validated module manifest");
}
ModuleIdentity ModuleInterceptor::identity(ModuleHandle m) const {
    impl_->current(); return impl_->module(m).identity;
}
ModuleStats ModuleInterceptor::statistics() const {
    const auto& s=*impl_; s.current(); auto stats=s.stats;
    stats.cache_entries=s.cache.size(); stats.live_modules=s.modules.size(); stats.live_functions=s.functions.size();
    return stats;
}
void ModuleInterceptor::clear_cache() {
    auto& s=*impl_; s.current(); s.cache.clear(); s.stats.cache_bytes=0;
}
void ModuleInterceptor::unload(ModuleHandle handle) {
    auto& s=*impl_; s.current(); auto& m=s.module(handle);
    check(cuModuleUnload(m.driver),"unload module");
    for (auto it=s.functions.begin();it!=s.functions.end();) {
        if (it->second.module==handle.value) it=s.functions.erase(it); else ++it;
    }
    s.modules.erase(handle.value);
}
Status ModuleInterceptor::launch(FunctionHandle handle,dim3 grid,dim3 block,const std::vector<void*>& args,
                                  OutputSpan output,cudaStream_t stream) {
    auto& s=*impl_; s.current();
    if (s.poisoned) throw std::logic_error("interceptor is terminal after a failed launch");
    const auto it=s.functions.find(handle.value);
    if (it==s.functions.end()) throw std::invalid_argument("foreign, stale or unknown function handle");
    const auto f=it->second; auto& m=s.module({f.module});
    s.runtime.view(); // Reject a Runtime poisoned by another interceptor before launching.
    if (grid.x!=1 || grid.y!=1 || grid.z!=1 || block.x!=s.view.threads || block.y!=1 || block.z!=1)
        throw std::invalid_argument("launch requires exactly the Runtime's one-dimensional CTA");
    if (args.size()!=f.parameters) throw std::invalid_argument("kernel parameter count mismatch");
    for (const auto arg:args) if (!arg) throw std::invalid_argument("null kernel parameter storage");
    const auto driver_stream=reinterpret_cast<CUstream>(stream);
    if (stream) {
        CUcontext stream_context=nullptr; check(cuStreamGetCtx(driver_stream,&stream_context),"stream context");
        if (stream_context!=s.context) throw std::invalid_argument("stream belongs to another CUDA context");
    }
    CUstreamCaptureStatus capture;
    check(cuStreamIsCapturing(driver_stream,&capture),"stream capture state");
    if (capture!=CU_STREAM_CAPTURE_STATUS_NONE) throw std::invalid_argument("graph capture is unsupported");
    // Ordinary preceding copies/work are legal. Never mistake their unfinished DMA
    // for a second protocol producer; this entry point serializes its own launches.
    check(cuStreamSynchronize(driver_stream),"finish preceding stream work");
    if ((output.data==nullptr)!=(output.bytes==0)) throw std::invalid_argument("output requires both pointer and length");
    PtxBinding binding{}; binding.view=s.view; binding.instance=m.identity.instance;
    binding.output_base=reinterpret_cast<std::uint64_t>(output.data); binding.output_bytes=output.bytes;
    if (output.bytes) {
        CUdeviceptr base=0; std::size_t bytes=0;
        check(cuMemGetAddressRange(&base,&bytes,binding.output_base),"output allocation");
        if (binding.output_base<base || output.bytes>bytes || binding.output_base-base>bytes-output.bytes)
            throw std::invalid_argument("output exceeds CUDA allocation");
    }
    try {
        // Pinned staging survives until serve/drain. Binding and kernel share the
        // selected stream, including nonblocking streams: no default-stream fence.
        *s.staging=binding;
        check(cuMemcpyHtoDAsync(m.binding,s.staging,sizeof(binding),driver_stream),"bind launch output");
        check(cuLaunchKernel(f.driver,1,1,1,block.x,1,1,0,driver_stream,const_cast<void**>(args.data()),nullptr),"launch rewritten kernel");
        const auto status=s.runtime.serve(stream);
        if (status!=Status::Ok) s.poisoned=true;
        return status;
    } catch (...) { s.poisoned=true; cuStreamSynchronize(driver_stream); throw; }
}
}
