#include "hbflab/async_module.hpp"
#include "async_ptx_helper_embedded.hpp"
#include <cuda.h>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <list>
#include <map>
#include <stdexcept>
#include <thread>

namespace hbflab {
static_assert(sizeof(AsyncDeviceView)==64 && alignof(AsyncDeviceView)==8,"async PTX hidden parameter ABI");
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
struct AsyncModuleInterceptor::Impl {
    struct Module {
        CUmodule driver=nullptr; ModuleIdentity identity;
        std::map<std::string,std::uint64_t> handles;
    };
    struct Function { std::uint64_t module; CUfunction driver; unsigned parameters; };
    struct Cached { std::string key, sha256; std::vector<char> binary; };
    LiveTimingService& service;
    CUcontext context=nullptr; CUdevice device=0; int sm=0, driver=0;
    unsigned long long context_id=0;
    const std::thread::id owner=std::this_thread::get_id();
    std::map<std::uint64_t,Module> modules;
    std::map<std::uint64_t,Function> functions;
    ModuleOptions options;
    ModuleStats stats;
    std::list<Cached> cache; // MRU at front; binaries only, never module bindings.
    const std::string helper_hash=sha256(embedded_async_ptx_helper);
    explicit Impl(LiveTimingService& r,ModuleOptions o):service(r),options(o) {
        service.statistics();
        check(cuCtxGetCurrent(&context),"current context");if(!context)throw std::runtime_error("CUDA context required");
        check(cuCtxGetId(context,&context_id),"context identity");check(cuCtxGetDevice(&device),"current device");
        int major=0,minor=0;check(cuDeviceGetAttribute(&major,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,device),"major");
        check(cuDeviceGetAttribute(&minor,CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,device),"minor");sm=major*10+minor;
        check(cuDriverGetVersion(&driver),"driver version");if(sm<75)throw std::runtime_error("SM75+ required");
    }
    ~Impl(){
        if(cuCtxPushCurrent(context)!=CUDA_SUCCESS)return;
        try{service.synchronize();}catch(...){}
        cuCtxSynchronize();for(const auto& m:modules)cuModuleUnload(m.second.driver);
        CUcontext ignored=nullptr;cuCtxPopCurrent(&ignored);
    }
    void current() const {
        if (owner!=std::this_thread::get_id()) throw std::logic_error("module interceptor used from another host thread");
        CUcontext actual=nullptr; check(cuCtxGetCurrent(&actual),"current context");
        if (actual!=context) throw std::logic_error("module interceptor used in a different CUDA context");
    }
    Module& module(AsyncModuleHandle handle) {
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
AsyncModuleInterceptor::AsyncModuleInterceptor(LiveTimingService& r,ModuleOptions o):impl_(std::make_unique<Impl>(r,o)) {}
AsyncModuleInterceptor::~AsyncModuleInterceptor()=default;
AsyncModuleHandle AsyncModuleInterceptor::load_ptx(std::string_view source) {
    auto& s=*impl_; s.current();
    s.service.statistics();
    const auto rewritten=rewrite_async_ptx(source); // Entire module validated before any JIT/load.
    if (rewritten.target_sm>static_cast<unsigned>(s.sm)) throw std::invalid_argument("PTX target exceeds current GPU architecture");
    ModuleIdentity identity;identity.frontend=async_ptx_frontend_version;identity.protocol=async_abi_version;
    identity.source_sha256=sha256(source); identity.rewritten_sha256=sha256(rewritten.text);
    identity.helper_sha256=s.helper_hash; identity.entries=rewritten.entries;
    identity.device=s.device; identity.target_sm=s.sm; identity.driver_version=s.driver;
    identity.context_id=s.context_id;
    identity.artifact_sha256=sha256("HBFLab-ASYNC-PTX\n"+std::to_string(async_ptx_frontend_version)+"\n"+std::to_string(async_abi_version)+"\n"+
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
        check(cuLinkAddData(link.value,CU_JIT_INPUT_PTX,const_cast<char*>(embedded_async_ptx_helper.c_str()),
            embedded_async_ptx_helper.size()+1,"hbflab-helper",0,nullptr,nullptr),"link helper",log);
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
        if (!compiled.empty()) s.cache_binary(identity.artifact_sha256,identity.cubin_sha256,compiled);
        s.modules.emplace(identity.instance,std::move(module));
    } catch (...) { cuModuleUnload(module.driver); throw; }
    return {identity.instance};
}
AsyncModuleHandle AsyncModuleInterceptor::load_file(const std::filesystem::path& path) {
    impl_->current();
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) throw std::invalid_argument("cannot open PTX file");
    const auto bytes=file.tellg();
    if (bytes<=0 || bytes>1024*1024) throw std::invalid_argument("PTX file size outside limit");
    std::string text(static_cast<std::size_t>(bytes),'\0'); file.seekg(0);
    if (!file.read(text.data(),static_cast<std::streamsize>(text.size()))) throw std::runtime_error("cannot read complete PTX file");
    return load_ptx(text);
}
AsyncFunctionHandle AsyncModuleInterceptor::function(AsyncModuleHandle handle,const std::string& name) {
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
ModuleIdentity AsyncModuleInterceptor::identity(AsyncModuleHandle m) const {
    impl_->current(); return impl_->module(m).identity;
}
ModuleStats AsyncModuleInterceptor::statistics() const {
    const auto& s=*impl_; s.current(); auto stats=s.stats;
    stats.cache_entries=s.cache.size(); stats.live_modules=s.modules.size(); stats.live_functions=s.functions.size();
    return stats;
}
void AsyncModuleInterceptor::clear_cache() {
    auto& s=*impl_; s.current(); s.cache.clear(); s.stats.cache_bytes=0;
}
void AsyncModuleInterceptor::unload(AsyncModuleHandle handle) {
    auto& s=*impl_; s.current(); auto& m=s.module(handle);
    s.service.synchronize();
    check(cuModuleUnload(m.driver),"unload module");
    for (auto it=s.functions.begin();it!=s.functions.end();) {
        if (it->second.module==handle.value) it=s.functions.erase(it); else ++it;
    }
    s.modules.erase(handle.value);
}
std::uint64_t AsyncModuleInterceptor::launch(AsyncFunctionHandle handle,AsyncLaunchShape shape,const std::vector<void*>& args,
                                              OutputSpan output,cudaStream_t stream,NativeReadSpan native) {
    auto& s=*impl_;s.current();const auto it=s.functions.find(handle.value);
    if(it==s.functions.end())throw std::invalid_argument("foreign/stale function");const auto f=it->second;auto& m=s.module({f.module});
    if(args.size()!=f.parameters)throw std::invalid_argument("parameter count mismatch");
    for(auto arg:args)if(!arg)throw std::invalid_argument("null parameter storage");
    if(stream){CUcontext c=nullptr;check(cuStreamGetCtx(reinterpret_cast<CUstream>(stream),&c),"stream context");if(c!=s.context)throw std::invalid_argument("foreign stream");}
    if((output.data==nullptr)!=(output.bytes==0))throw std::invalid_argument("invalid output span");
    std::uint64_t base=reinterpret_cast<std::uint64_t>(output.data),bytes=output.bytes,instance=m.identity.instance;
    if(bytes){CUdeviceptr allocation;std::size_t size;CUcontext c=nullptr;
        check(cuMemGetAddressRange(&allocation,&size,base),"output allocation");
        check(cuPointerGetAttribute(&c,CU_POINTER_ATTRIBUTE_CONTEXT,base),"output context");
        if(c!=s.context || base<allocation || bytes>size || base-allocation>size-bytes)throw std::invalid_argument("output bounds/context");}
    std::uint64_t native_base=reinterpret_cast<std::uint64_t>(native.data),native_bytes=native.bytes;
    if((native.data==nullptr)!=(native.bytes==0))throw std::invalid_argument("invalid native input span");
    if(native_bytes){CUdeviceptr allocation;std::size_t size;CUcontext c=nullptr;
        check(cuMemGetAddressRange(&allocation,&size,native_base),"native input allocation");
        check(cuPointerGetAttribute(&c,CU_POINTER_ATTRIBUTE_CONTEXT,native_base),"native input context");
        if(c!=s.context || native_base<allocation || native_bytes>size || native_base-allocation>size-native_bytes)throw std::invalid_argument("native input bounds/context");
        if(bytes && (base>=native_base?base-native_base<native_bytes:native_base-base<bytes))throw std::invalid_argument("native input/output overlap");}
    std::vector<void*> actual{&base,&bytes,&instance,&native_base,&native_bytes};actual.insert(actual.end(),args.begin(),args.end());
    return s.service.launch_driver(shape,reinterpret_cast<std::uintptr_t>(f.driver),actual,stream);
}
}
