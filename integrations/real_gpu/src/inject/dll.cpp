#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <detours.h>
#include <cuda.h>
#include "hbflab/module.hpp"
#include "hbflab/async_module.hpp"
#include "hbflab/injection.h"
#include <cstdio>
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace {
thread_local unsigned internal=0;
void worker_enter() noexcept {++internal;}
void worker_leave() noexcept {--internal;}
SRWLOCK lock=SRWLOCK_INIT;
HBFLabInjectionStats counters{1,0,0,0,0,0,0};
std::atomic<bool> driver_ready{false};
struct Guard {
    Guard(){AcquireSRWLockExclusive(&lock); ++internal;}
    ~Guard(){--internal; ReleaseSRWLockExclusive(&lock);}
};
struct Failure:std::runtime_error {
    CUresult code;
    Failure(CUresult c,const char* message):std::runtime_error(message),code(c){}
};
void require(bool ok,CUresult code,const char* text){if(!ok) throw Failure(code,text);}
void driver(CUresult c){if(c!=CUDA_SUCCESS) throw Failure(c,"CUDA operation rejected");}
void copy(void* to,const void* from,std::size_t bytes) {
    SIZE_T got=0;
    require(from && ReadProcessMemory(GetCurrentProcess(),from,to,bytes,&got) && got==bytes,
        CUDA_ERROR_INVALID_VALUE,"unreadable application argument");
}
template<class T> void publish(T* to,const T& value) {
    SIZE_T written=0;
    require(to && WriteProcessMemory(GetCurrentProcess(),to,&value,sizeof(value),&written) && written==sizeof(value),
        CUDA_ERROR_INVALID_VALUE,"unwritable application result");
}
std::string text(const void* source,std::size_t limit) {
    require(source!=nullptr,CUDA_ERROR_INVALID_VALUE,"null text image");
    std::string result;
    auto address=reinterpret_cast<std::uintptr_t>(source);
    while(result.size()<=limit) {
        char bytes[4096];
        const auto count=(std::min)(sizeof(bytes)-(address%sizeof(bytes)),limit+1-result.size());
        copy(bytes,reinterpret_cast<const void*>(address),count);
        for(std::size_t i=0;i<count;++i) {
            if(!bytes[i]) return result;
            result+=bytes[i];
        }
        address+=count;
    }
    throw Failure(CUDA_ERROR_INVALID_VALUE,"text exceeds injection limit");
}
std::string env(const char* name) {
    char value[256]; const auto n=GetEnvironmentVariableA(name,value,sizeof(value));
    require(n && n<sizeof(value),CUDA_ERROR_INVALID_VALUE,"missing/oversized injection configuration");
    return {value,n};
}
std::uint64_t number(const char* name,std::uint64_t maximum) {
    const auto value=env(name); std::size_t used=0;
    require(value.find_first_not_of("0123456789")==std::string::npos,CUDA_ERROR_INVALID_VALUE,"invalid integer configuration");
    const auto n=std::stoull(value,&used);
    require(used==value.size() && n<=maximum,CUDA_ERROR_INVALID_VALUE,"configuration exceeds limit"); return n;
}
struct PendingModule {
    std::string source;
    hbflab::RewrittenPtx manifest;
    hbflab::ModuleHandle actual{};
    hbflab::AsyncModuleHandle async_actual{};
    std::map<std::string,CUfunction> names;
};
struct PendingFunction { CUmodule module; std::string name; hbflab::FunctionHandle actual{}; hbflab::AsyncFunctionHandle async_actual{}; };
struct State {
    std::string entry=env("HBFLAB_INJECT_ENTRY");
    unsigned input_arg=static_cast<unsigned>(number("HBFLAB_INJECT_INPUT_ARG",63));
    unsigned output_arg=static_cast<unsigned>(number("HBFLAB_INJECT_OUTPUT_ARG",63));
    std::size_t input_bytes=number("HBFLAB_INJECT_INPUT_BYTES",1024*1024*1024ull);
    std::size_t output_bytes=number("HBFLAB_INJECT_OUTPUT_BYTES",1024*1024*1024ull);
    hbflab::RuntimeOptions options{};
    DWORD owner=0;
    CUcontext context=nullptr;
    std::uintptr_t next=0x10000;
    CUdeviceptr input=0,allocation=0;
    std::map<CUmodule,PendingModule> modules;
    std::map<CUfunction,PendingFunction> functions;
    std::unique_ptr<hbflab::Runtime> runtime;
    std::unique_ptr<hbflab::ModuleInterceptor> loader;
    bool asynchronous=false;
    unsigned slots=0,launch_limit=0;
    bool has_native=false;unsigned native_arg=0;std::size_t native_bytes=0;
    std::uint64_t accounted=0;
    std::unique_ptr<hbflab::LiveTimingService> service;
    std::unique_ptr<hbflab::AsyncModuleInterceptor> async_loader;
    State() {
        require(input_arg!=output_arg && input_bytes && output_bytes,CUDA_ERROR_INVALID_VALUE,"input/output roles must be distinct and nonempty");
        options.threads=static_cast<unsigned>(number("HBFLAB_INJECT_THREADS",256));
        require(options.threads!=0,CUDA_ERROR_INVALID_VALUE,"zero threads");
        const auto mode=env("HBFLAB_INJECT_MODE");
        require(mode=="thread" || mode=="warp",CUDA_ERROR_INVALID_VALUE,"producer mode must be thread or warp");
        options.mode=mode=="warp"?hbflab::ProducerMode::Warp:hbflab::ProducerMode::Thread;
        const auto protocol=env("HBFLAB_INJECT_PROTOCOL");
        require(protocol=="collective" || protocol=="async",CUDA_ERROR_INVALID_VALUE,"invalid protocol");asynchronous=protocol=="async";
        slots=static_cast<unsigned>(number("HBFLAB_INJECT_SLOTS",8192));launch_limit=static_cast<unsigned>(number("HBFLAB_INJECT_LAUNCHES",256));
        require(!asynchronous || (mode=="thread" && slots>=options.threads && launch_limit),CUDA_ERROR_INVALID_VALUE,"invalid async producer pool");
        has_native=GetEnvironmentVariableA("HBFLAB_INJECT_NATIVE_ARG",nullptr,0)!=0;
        if(has_native){native_arg=static_cast<unsigned>(number("HBFLAB_INJECT_NATIVE_ARG",63));native_bytes=number("HBFLAB_INJECT_NATIVE_BYTES",1024*1024*1024ull);
            require(asynchronous && native_bytes && native_arg!=input_arg && native_arg!=output_arg,CUDA_ERROR_INVALID_VALUE,"invalid native input role");}
    }
    void current() {
        CUcontext now=nullptr; driver(cuCtxGetCurrent(&now));
        require(now!=nullptr,CUDA_ERROR_INVALID_CONTEXT,"no current context");
        if(!owner || (modules.empty() && !runtime && !service)) { owner=GetCurrentThreadId(); context=now; }
        require(owner==GetCurrentThreadId(),CUDA_ERROR_NOT_SUPPORTED,"one host thread owns the injected session");
        require(context==now,CUDA_ERROR_INVALID_CONTEXT,"injected session context changed");
    }
    template<class T> T token(unsigned tag) {
        require(next<(UINTPTR_MAX>>4),CUDA_ERROR_OUT_OF_MEMORY,"injection handle space exhausted");
        return reinterpret_cast<T>((next++<<4)|tag);
    }
    PendingModule& module(CUmodule m) {
        const auto it=modules.find(m);
        require(it!=modules.end(),CUDA_ERROR_INVALID_HANDLE,"foreign/stale module handle"); return it->second;
    }
    void reset() {
        async_loader.reset();service.reset();accounted=0;
        loader.reset(); runtime.reset(); input=allocation=0; owner=0; context=nullptr;
    }
    void synchronize() {
        if(service){service->synchronize();const auto n=service->statistics().completed;counters.requests+=n-accounted;accounted=n;}
    }
};
State* session=nullptr; // Never run CUDA destructors from DllMain/loader lock.
State& state() {
    if(!session) session=new State;
    session->current(); return *session;
}
template<class F> CUresult invoke(F f) noexcept {
    Guard guard;
    try { return f(); }
    catch(const Failure& e){++counters.rejected; std::fprintf(stderr,"[HBFLab inject] %s (%d)\n",e.what(),e.code); return e.code;}
    catch(const std::invalid_argument& e){++counters.rejected; std::fprintf(stderr,"[HBFLab inject] %s\n",e.what()); return CUDA_ERROR_INVALID_VALUE;}
    catch(const std::exception& e){++counters.rejected; std::fprintf(stderr,"[HBFLab inject] %s\n",e.what()); return CUDA_ERROR_UNKNOWN;}
    catch(...){++counters.rejected; return CUDA_ERROR_UNKNOWN;}
}

using Load=CUresult(CUDAAPI*)(CUmodule*,const char*);
using LoadData=CUresult(CUDAAPI*)(CUmodule*,const void*);
using LoadEx=CUresult(CUDAAPI*)(CUmodule*,const void*,unsigned,CUjit_option*,void**);
using GetFunction=CUresult(CUDAAPI*)(CUfunction*,CUmodule,const char*);
using Unload=CUresult(CUDAAPI*)(CUmodule);
using Launch=CUresult(CUDAAPI*)(CUfunction,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned,CUstream,void**,void**);
using Proc1=CUresult(CUDAAPI*)(const char*,void**,int,cuuint64_t);
using Proc2=CUresult(CUDAAPI*)(const char*,void**,int,cuuint64_t,CUdriverProcAddressQueryResult*);
Load real_load=nullptr;
LoadData real_data=nullptr,real_fatbin=nullptr;
LoadEx real_ex=nullptr;
GetFunction real_function=nullptr;
Unload real_unload=nullptr;
Launch real_launch=nullptr,real_launch_ptsz=nullptr;
Proc1 real_proc1=nullptr;
Proc2 real_proc2=nullptr;
decltype(&cuModuleGetGlobal) real_global=nullptr;
decltype(&cuFuncGetAttribute) real_attribute=nullptr;
decltype(&cuGetExportTable) real_export=nullptr;
decltype(&cuLaunchKernelEx) real_launch_ex=nullptr,real_launch_ex_ptsz=nullptr;
decltype(&cuLaunchCooperativeKernel) real_cooperative=nullptr,real_cooperative_ptsz=nullptr;
decltype(&cuLibraryLoadData) real_library_data=nullptr;
decltype(&cuLibraryLoadFromFile) real_library_file=nullptr;
decltype(&cuMemFree) real_free=nullptr;
decltype(&cuCtxDestroy) real_destroy=nullptr;
decltype(&cuDevicePrimaryCtxRelease) real_release=nullptr;
decltype(&cuDevicePrimaryCtxReset) real_reset=nullptr;
decltype(&cuMemcpyHtoD) real_htod=nullptr;
decltype(&cuMemcpyHtoDAsync) real_htod_async=nullptr;
decltype(&cuMemcpyDtoD) real_dtod=nullptr;
decltype(&cuMemcpyDtoDAsync) real_dtod_async=nullptr;
decltype(&cuMemcpyHtoDAsync) real_htod_async_ptsz=nullptr;
decltype(&cuMemcpyDtoDAsync) real_dtod_async_ptsz=nullptr;
decltype(&cuMemcpy) real_copy=nullptr;
decltype(&cuMemcpyHtoD) real_htod_ptds=nullptr;
decltype(&cuMemcpyDtoD) real_dtod_ptds=nullptr;
decltype(&cuMemcpy) real_copy_ptds=nullptr;
decltype(&cuMemcpyAsync) real_copy_async=nullptr,real_copy_async_ptsz=nullptr;
decltype(&cuGraphAddKernelNode) real_graph_kernel=nullptr;
decltype(&cuCtxSynchronize) real_ctx_sync=nullptr;
decltype(&cuStreamSynchronize) real_stream_sync=nullptr,real_stream_sync_ptsz=nullptr;
decltype(&cuMemcpyDtoH) real_dtoh=nullptr,real_dtoh_ptds=nullptr;
decltype(&cuEventSynchronize) real_event_sync=nullptr;
decltype(&cuEventQuery) real_event_query=nullptr;
decltype(&cuStreamQuery) real_stream_query=nullptr,real_stream_query_ptsz=nullptr;
decltype(&cuStreamDestroy) real_stream_destroy=nullptr;
#define DECLARE_SET(bits) \
decltype(&cuMemsetD##bits) real_set##bits=nullptr,real_set##bits##_ptds=nullptr; \
decltype(&cuMemsetD##bits##Async) real_set##bits##_async=nullptr,real_set##bits##_async_ptsz=nullptr;
DECLARE_SET(8)
DECLARE_SET(16)
DECLARE_SET(32)

CUresult load_source(CUmodule* out,std::string source) {
    auto& s=state(); require(out!=nullptr,CUDA_ERROR_INVALID_VALUE,"null module output");
    std::size_t retained=source.size();for(const auto& m:s.modules)retained+=m.second.source.size();
    require(s.modules.size()<32 && retained<=8*1024*1024,CUDA_ERROR_OUT_OF_MEMORY,"pending module budget exceeded");
    PendingModule module; module.manifest=s.asynchronous?hbflab::rewrite_async_ptx(source):hbflab::rewrite_ptx(source); module.source=std::move(source);
    bool selected=false;
    for(const auto& entry:module.manifest.entries) if(entry.name==s.entry) {
        const auto& p=entry.parameter_bytes;
        require(s.input_arg<p.size() && s.output_arg<p.size() && p[s.input_arg]==8 && p[s.output_arg]==8,
            CUDA_ERROR_INVALID_VALUE,"configured pointer roles must name 64-bit parameters"); selected=true;
        require(!s.has_native || (s.native_arg<p.size() && p[s.native_arg]==8),CUDA_ERROR_INVALID_VALUE,"native role must name 64-bit parameter");
    }
    require(selected,CUDA_ERROR_NOT_SUPPORTED,"configured entry absent from PTX module");
    const auto token=s.token<CUmodule>(1);
    s.modules.emplace(token,std::move(module));
    try { publish(out,token); } catch(...) {s.modules.erase(token); throw;}
    ++counters.module_loads;
    std::fprintf(stderr,"[HBFLab inject] accepted PTX module %p\n",static_cast<void*>(token));
    return CUDA_SUCCESS;
}
CUresult CUDAAPI hook_load(CUmodule* out,const char* path) {
    if(internal) return real_load(out,path);
    return invoke([&]{
        const auto name=text(path,32767); std::ifstream f(name,std::ios::binary|std::ios::ate);
        require(bool(f),CUDA_ERROR_FILE_NOT_FOUND,"cannot open PTX file"); const auto n=f.tellg();
        require(n>0 && n<=1024*1024,CUDA_ERROR_INVALID_VALUE,"PTX file exceeds size limit");
        std::string source(static_cast<std::size_t>(n),'\0'); f.seekg(0);
        require(bool(f.read(source.data(),static_cast<std::streamsize>(source.size()))),CUDA_ERROR_INVALID_VALUE,"incomplete PTX file");
        return load_source(out,std::move(source));
    });
}
CUresult CUDAAPI hook_data(CUmodule* out,const void* image) {
    if(internal) return real_data(out,image);
    return invoke([&]{return load_source(out,text(image,1024*1024));});
}
CUresult CUDAAPI hook_ex(CUmodule* out,const void* image,unsigned n,CUjit_option* options,void** values) {
    if(internal) return real_ex(out,image,n,options,values);
    return invoke([&]{require(n==0,CUDA_ERROR_NOT_SUPPORTED,"application JIT options are unsupported"); return load_source(out,text(image,1024*1024));});
}
CUresult CUDAAPI hook_fatbin(CUmodule* out,const void* image) {
    if(internal) return real_fatbin(out,image);
    return invoke([]()->CUresult{throw Failure(CUDA_ERROR_NOT_SUPPORTED,"fatbin injection is unsupported");});
}
CUresult CUDAAPI hook_function(CUfunction* out,CUmodule handle,const char* name) {
    if(internal) return real_function(out,handle,name);
    return invoke([&]{
        auto& s=state(); auto& m=s.module(handle); const auto entry=text(name,1024);
        require(entry==s.entry,CUDA_ERROR_NOT_SUPPORTED,"entry outside injection configuration");
        auto it=m.names.find(entry);
        if(it==m.names.end()) {
            const auto f=s.token<CUfunction>(2); s.functions.emplace(f,PendingFunction{handle,entry,{}});
            try {it=m.names.emplace(entry,f).first;} catch(...) {s.functions.erase(f);throw;}
        }
        publish(out,it->second); return CUDA_SUCCESS;
    });
}
CUresult CUDAAPI hook_unload(CUmodule handle) {
    if(internal) return real_unload(handle);
    return invoke([&]{
        auto& s=state(); auto& m=s.module(handle);
        if(m.async_actual.value){s.synchronize();s.async_loader->unload(m.async_actual);}
        if(m.actual.value) s.loader->unload(m.actual);
        for(auto it=s.functions.begin();it!=s.functions.end();) {
            if(it->second.module==handle) it=s.functions.erase(it); else ++it;
        }
        s.modules.erase(handle); if(s.modules.empty()) s.reset(); return CUDA_SUCCESS;
    });
}
CUresult launch(CUfunction function,unsigned gx,unsigned gy,unsigned gz,unsigned bx,unsigned by,unsigned bz,
                unsigned shared,CUstream stream,void** parameters,void** extra) {
    auto& s=state(); auto it=s.functions.find(function);
    require(it!=s.functions.end(),CUDA_ERROR_INVALID_HANDLE,"foreign/stale function handle");
    require((s.asynchronous?(gx && std::uint64_t{gx}*bx<=s.slots):gx==1) && gy==1 && gz==1 && bx==s.options.threads && by==1 && bz==1 && !shared && !extra,
        CUDA_ERROR_NOT_SUPPORTED,"injection requires configured single CTA, no shared bytes or packed arguments");
    auto& f=it->second; auto& m=s.module(f.module);
    std::vector<std::uint32_t> widths;for(const auto& e:m.manifest.entries)if(e.name==f.name)widths.assign(e.parameter_bytes.begin(),e.parameter_bytes.end());
    std::vector<void*> args(widths.size()); copy(args.data(),parameters,args.size()*sizeof(void*));
    std::vector<std::vector<unsigned char>> storage(widths.size());
    for(std::size_t i=0;i<args.size();++i){storage[i].resize(widths[i]);copy(storage[i].data(),args[i],widths[i]);args[i]=storage[i].data();}
    CUdeviceptr input=0,output=0; copy(&input,args[s.input_arg],sizeof(input)); copy(&output,args[s.output_arg],sizeof(output));
    require(input && output,CUDA_ERROR_INVALID_VALUE,"null device input/output");
    CUcontext stream_context=s.context;
    if(stream) driver(cuStreamGetCtx(stream,&stream_context));
    require(stream_context==s.context,CUDA_ERROR_INVALID_CONTEXT,"stream belongs to another context");
    CUstreamCaptureStatus capture; driver(cuStreamIsCapturing(stream,&capture));
    require(capture==CU_STREAM_CAPTURE_STATUS_NONE,CUDA_ERROR_NOT_SUPPORTED,"graph capture is unsupported");
    if(!s.asynchronous || !s.service)driver(cuStreamSynchronize(stream));
    if(!s.runtime && !s.service) {
        CUdevice device; driver(cuCtxGetDevice(&device)); CUcontext primary=nullptr;
        driver(cuDevicePrimaryCtxRetain(&primary,device)); driver(cuDevicePrimaryCtxRelease(device));
        require(primary==s.context,CUDA_ERROR_NOT_SUPPORTED,"injection currently requires the primary CUDA context");
        CUdeviceptr allocation=0; std::size_t bytes=0; driver(cuMemGetAddressRange(&allocation,&bytes,input));
        if(s.asynchronous) {
            hbflab::AsyncRuntimeOptions options;options.timeout_ns=1000000000;
            auto service=std::make_unique<hbflab::LiveTimingService>(options,s.slots,s.launch_limit,hbflab::LiveWorkerHooks{worker_enter,worker_leave});
            service->register_readonly(reinterpret_cast<void*>(input),s.input_bytes);service->start();
            auto loader=std::make_unique<hbflab::AsyncModuleInterceptor>(*service);
            s.service=std::move(service);s.async_loader=std::move(loader);
        }else {
            auto runtime=std::make_unique<hbflab::Runtime>(s.options);
            runtime->register_readonly(reinterpret_cast<void*>(input),s.input_bytes);
            auto loader=std::make_unique<hbflab::ModuleInterceptor>(*runtime);
            s.runtime=std::move(runtime);s.loader=std::move(loader);
        }
        s.input=input; s.allocation=allocation;
    }
    require(input==s.input,CUDA_ERROR_INVALID_VALUE,"readonly input changed while modules are live");
    if(s.asynchronous) {
        if(!m.async_actual.value)m.async_actual=s.async_loader->load_ptx(m.source);
        if(!f.async_actual.value)f.async_actual=s.async_loader->function(m.async_actual,f.name);
        CUdeviceptr native=0;if(s.has_native)copy(&native,args[s.native_arg],sizeof(native));
        s.async_loader->launch(f.async_actual,{gx,bx},args,{reinterpret_cast<void*>(output),s.output_bytes},reinterpret_cast<cudaStream_t>(stream),{reinterpret_cast<void*>(native),s.native_bytes});
        ++counters.launches;return CUDA_SUCCESS;
    }
    if(!m.actual.value) m.actual=s.loader->load_ptx(m.source);
    if(!f.actual.value) f.actual=s.loader->function(m.actual,f.name);
    const auto before=s.runtime->statistics().requests;
    const auto status=s.loader->launch(f.actual,dim3(1),dim3(bx),args,
        {reinterpret_cast<void*>(output),s.output_bytes},reinterpret_cast<cudaStream_t>(stream));
    ++counters.launches; counters.requests+=s.runtime->statistics().requests-before;
    std::fprintf(stderr,"[HBFLab inject] served module=%p requests=%llu status=%u\n",static_cast<void*>(f.module),
        static_cast<unsigned long long>(counters.requests),static_cast<unsigned>(status));
    return status==hbflab::Status::Ok?CUDA_SUCCESS:CUDA_ERROR_LAUNCH_FAILED;
}
CUresult CUDAAPI hook_launch(CUfunction f,unsigned gx,unsigned gy,unsigned gz,unsigned bx,unsigned by,unsigned bz,unsigned sh,CUstream st,void** p,void** e) {
    if(internal) return real_launch(f,gx,gy,gz,bx,by,bz,sh,st,p,e);
    return invoke([&]{return launch(f,gx,gy,gz,bx,by,bz,sh,st,p,e);});
}
CUresult CUDAAPI hook_launch_ptsz(CUfunction f,unsigned gx,unsigned gy,unsigned gz,unsigned bx,unsigned by,unsigned bz,unsigned sh,CUstream st,void** p,void** e) {
    if(internal) return real_launch_ptsz(f,gx,gy,gz,bx,by,bz,sh,st,p,e);
    return invoke([&]{return launch(f,gx,gy,gz,bx,by,bz,sh,st?st:CU_STREAM_PER_THREAD,p,e);});
}
CUresult unsupported(){return invoke([]()->CUresult{throw Failure(CUDA_ERROR_NOT_SUPPORTED,"API outside injected Driver subset");});}
CUresult CUDAAPI hook_global(CUdeviceptr* p,std::size_t* n,CUmodule m,const char* name){return internal?real_global(p,n,m,name):unsupported();}
CUresult CUDAAPI hook_attribute(int* v,CUfunction_attribute a,CUfunction f){return internal?real_attribute(v,a,f):unsupported();}
CUresult CUDAAPI hook_export(const void** p,const CUuuid* id){return internal?real_export(p,id):unsupported();}
CUresult CUDAAPI hook_launch_ex(const CUlaunchConfig* c,CUfunction f,void** p,void** e){return internal?real_launch_ex(c,f,p,e):unsupported();}
CUresult CUDAAPI hook_launch_ex_ptsz(const CUlaunchConfig* c,CUfunction f,void** p,void** e){return internal?real_launch_ex_ptsz(c,f,p,e):unsupported();}
CUresult CUDAAPI hook_cooperative(CUfunction f,unsigned gx,unsigned gy,unsigned gz,unsigned bx,unsigned by,unsigned bz,unsigned sh,CUstream st,void** p){return internal?real_cooperative(f,gx,gy,gz,bx,by,bz,sh,st,p):unsupported();}
CUresult CUDAAPI hook_cooperative_ptsz(CUfunction f,unsigned gx,unsigned gy,unsigned gz,unsigned bx,unsigned by,unsigned bz,unsigned sh,CUstream st,void** p){return internal?real_cooperative_ptsz(f,gx,gy,gz,bx,by,bz,sh,st,p):unsupported();}
CUresult CUDAAPI hook_library_data(CUlibrary* l,const void* p,CUjit_option* o,void** v,unsigned n,CUlibraryOption* lo,void** lv,unsigned ln){return internal?real_library_data(l,p,o,v,n,lo,lv,ln):unsupported();}
CUresult CUDAAPI hook_library_file(CUlibrary* l,const char* p,CUjit_option* o,void** v,unsigned n,CUlibraryOption* lo,void** lv,unsigned ln){return internal?real_library_file(l,p,o,v,n,lo,lv,ln):unsupported();}
bool borrowed(CUdeviceptr address,std::size_t bytes) {
    if(!session || (!session->runtime && !session->service) || !bytes) return false;
    return address>=session->input?address-session->input<session->input_bytes:session->input-address<bytes;
}
CUresult CUDAAPI hook_free(CUdeviceptr p){
    if(internal) return real_free(p);
    return invoke([&]{require(!session || (!session->runtime && !session->service) || p!=session->allocation,CUDA_ERROR_NOT_PERMITTED,"cannot free a borrowed input before unloading modules");return real_free(p);});
}
void sync_session(){if(session && session->service){session->current();session->synchronize();}}
CUresult CUDAAPI hook_ctx_sync(){if(internal)return real_ctx_sync();return invoke([]{sync_session();return real_ctx_sync();});}
CUresult CUDAAPI hook_stream_sync(CUstream stream){if(internal)return real_stream_sync(stream);return invoke([&]{sync_session();return real_stream_sync(stream);});}
CUresult CUDAAPI hook_stream_sync_ptsz(CUstream stream){if(internal)return real_stream_sync_ptsz(stream);return invoke([&]{sync_session();return real_stream_sync_ptsz(stream);});}
CUresult CUDAAPI hook_dtoh(void* d,CUdeviceptr s,std::size_t n){if(internal)return real_dtoh(d,s,n);return invoke([&]{sync_session();return real_dtoh(d,s,n);});}
CUresult CUDAAPI hook_dtoh_ptds(void* d,CUdeviceptr s,std::size_t n){if(internal)return real_dtoh_ptds(d,s,n);return invoke([&]{sync_session();return real_dtoh_ptds(d,s,n);});}
void check_session(){if(session && session->service){session->current();session->service->check_status();}}
CUresult CUDAAPI hook_event_sync(CUevent event){if(internal)return real_event_sync(event);return invoke([&]{sync_session();return real_event_sync(event);});}
CUresult CUDAAPI hook_event_query(CUevent event){if(internal)return real_event_query(event);return invoke([&]{check_session();const auto r=real_event_query(event);if(r==CUDA_SUCCESS)check_session();return r;});}
CUresult CUDAAPI hook_stream_query(CUstream stream){if(internal)return real_stream_query(stream);return invoke([&]{check_session();const auto r=real_stream_query(stream);if(r==CUDA_SUCCESS)check_session();return r;});}
CUresult CUDAAPI hook_stream_query_ptsz(CUstream stream){if(internal)return real_stream_query_ptsz(stream);return invoke([&]{check_session();const auto r=real_stream_query_ptsz(stream);if(r==CUDA_SUCCESS)check_session();return r;});}
CUresult CUDAAPI hook_stream_destroy(CUstream stream){if(internal)return real_stream_destroy(stream);return invoke([&]{sync_session();return real_stream_destroy(stream);});}
CUresult CUDAAPI hook_destroy(CUcontext c){if(internal)return real_destroy(c);return invoke([&]{require(!session || session->modules.empty(),CUDA_ERROR_NOT_PERMITTED,"unload injected modules before destroying context");return real_destroy(c);});}
CUresult CUDAAPI hook_release(CUdevice d){if(internal)return real_release(d);return invoke([&]{require(!session || session->modules.empty(),CUDA_ERROR_NOT_PERMITTED,"unload injected modules before releasing context");return real_release(d);});}
CUresult CUDAAPI hook_reset(CUdevice d){if(internal)return real_reset(d);return invoke([&]{require(!session || session->modules.empty(),CUDA_ERROR_NOT_PERMITTED,"unload injected modules before resetting context");return real_reset(d);});}
CUresult CUDAAPI hook_htod(CUdeviceptr d,const void* p,std::size_t n){if(internal)return real_htod(d,p,n);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_htod(d,p,n);});}
CUresult CUDAAPI hook_htod_async(CUdeviceptr d,const void* p,std::size_t n,CUstream st){if(internal)return real_htod_async(d,p,n,st);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_htod_async(d,p,n,st);});}
CUresult CUDAAPI hook_dtod(CUdeviceptr d,CUdeviceptr p,std::size_t n){if(internal)return real_dtod(d,p,n);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_dtod(d,p,n);});}
CUresult CUDAAPI hook_dtod_async(CUdeviceptr d,CUdeviceptr p,std::size_t n,CUstream st){if(internal)return real_dtod_async(d,p,n,st);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_dtod_async(d,p,n,st);});}
CUresult CUDAAPI hook_htod_async_ptsz(CUdeviceptr d,const void* p,std::size_t n,CUstream st){if(internal)return real_htod_async_ptsz(d,p,n,st);return hook_htod_async(d,p,n,st?st:CU_STREAM_PER_THREAD);}
CUresult CUDAAPI hook_dtod_async_ptsz(CUdeviceptr d,CUdeviceptr p,std::size_t n,CUstream st){if(internal)return real_dtod_async_ptsz(d,p,n,st);return hook_dtod_async(d,p,n,st?st:CU_STREAM_PER_THREAD);}
CUresult CUDAAPI hook_copy(CUdeviceptr d,CUdeviceptr p,std::size_t n){if(internal)return real_copy(d,p,n);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_copy(d,p,n);});}
CUresult CUDAAPI hook_copy_async(CUdeviceptr d,CUdeviceptr p,std::size_t n,CUstream st){if(internal)return real_copy_async(d,p,n,st);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_copy_async(d,p,n,st);});}
CUresult CUDAAPI hook_copy_async_ptsz(CUdeviceptr d,CUdeviceptr p,std::size_t n,CUstream st){if(internal)return real_copy_async_ptsz(d,p,n,st);return hook_copy_async(d,p,n,st?st:CU_STREAM_PER_THREAD);}
CUresult CUDAAPI hook_htod_ptds(CUdeviceptr d,const void* p,std::size_t n){if(internal)return real_htod_ptds(d,p,n);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_htod_ptds(d,p,n);});}
CUresult CUDAAPI hook_dtod_ptds(CUdeviceptr d,CUdeviceptr p,std::size_t n){if(internal)return real_dtod_ptds(d,p,n);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_dtod_ptds(d,p,n);});}
CUresult CUDAAPI hook_copy_ptds(CUdeviceptr d,CUdeviceptr p,std::size_t n){if(internal)return real_copy_ptds(d,p,n);return invoke([&]{require(!borrowed(d,n),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_copy_ptds(d,p,n);});}
CUresult CUDAAPI hook_graph_kernel(CUgraphNode* n,CUgraph g,const CUgraphNode* d,std::size_t count,const CUDA_KERNEL_NODE_PARAMS* p){return internal?real_graph_kernel(n,g,d,count,p):unsupported();}
#define DEFINE_SET(bits,type) \
CUresult CUDAAPI hook_set##bits(CUdeviceptr d,type v,std::size_t n){ \
    if(internal)return real_set##bits(d,v,n); \
    return invoke([&]{require(n<=SIZE_MAX/(bits/8) && !borrowed(d,n*(bits/8)),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_set##bits(d,v,n);});} \
CUresult CUDAAPI hook_set##bits##_ptds(CUdeviceptr d,type v,std::size_t n){ \
    if(internal)return real_set##bits##_ptds(d,v,n); \
    return invoke([&]{require(n<=SIZE_MAX/(bits/8) && !borrowed(d,n*(bits/8)),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_set##bits##_ptds(d,v,n);});} \
CUresult CUDAAPI hook_set##bits##_async(CUdeviceptr d,type v,std::size_t n,CUstream st){ \
    if(internal)return real_set##bits##_async(d,v,n,st); \
    return invoke([&]{require(n<=SIZE_MAX/(bits/8) && !borrowed(d,n*(bits/8)),CUDA_ERROR_NOT_PERMITTED,"write to borrowed readonly input");return real_set##bits##_async(d,v,n,st);});} \
CUresult CUDAAPI hook_set##bits##_async_ptsz(CUdeviceptr d,type v,std::size_t n,CUstream st){ \
    if(internal)return real_set##bits##_async_ptsz(d,v,n,st);return hook_set##bits##_async(d,v,n,st?st:CU_STREAM_PER_THREAD);}
DEFINE_SET(8,unsigned char)
DEFINE_SET(16,unsigned short)
DEFINE_SET(32,unsigned)

struct Hook {const char* name; void** real; void* replacement;};
#define HOOK(symbol,real,hook) {symbol,reinterpret_cast<void**>(&real),reinterpret_cast<void*>(&hook)}
CUresult CUDAAPI hook_proc1(const char*,void**,int,cuuint64_t);
CUresult CUDAAPI hook_proc2(const char*,void**,int,cuuint64_t,CUdriverProcAddressQueryResult*);
Hook hooks[]={
    HOOK("cuEventSynchronize",real_event_sync,hook_event_sync),HOOK("cuEventQuery",real_event_query,hook_event_query),
    HOOK("cuStreamQuery",real_stream_query,hook_stream_query),HOOK("cuStreamQuery_ptsz",real_stream_query_ptsz,hook_stream_query_ptsz),
    HOOK("cuStreamDestroy_v2",real_stream_destroy,hook_stream_destroy),
    HOOK("cuCtxSynchronize",real_ctx_sync,hook_ctx_sync),HOOK("cuStreamSynchronize",real_stream_sync,hook_stream_sync),
    HOOK("cuStreamSynchronize_ptsz",real_stream_sync_ptsz,hook_stream_sync_ptsz),
    HOOK("cuMemcpyDtoH_v2",real_dtoh,hook_dtoh),HOOK("cuMemcpyDtoH_v2_ptds",real_dtoh_ptds,hook_dtoh_ptds),
    HOOK("cuModuleLoad",real_load,hook_load),HOOK("cuModuleLoadData",real_data,hook_data),
    HOOK("cuModuleLoadDataEx",real_ex,hook_ex),HOOK("cuModuleLoadFatBinary",real_fatbin,hook_fatbin),
    HOOK("cuModuleGetFunction",real_function,hook_function),HOOK("cuModuleUnload",real_unload,hook_unload),
    HOOK("cuLaunchKernel",real_launch,hook_launch),HOOK("cuLaunchKernel_ptsz",real_launch_ptsz,hook_launch_ptsz),
    HOOK("cuGetProcAddress",real_proc1,hook_proc1),HOOK("cuGetProcAddress_v2",real_proc2,hook_proc2),
    HOOK("cuModuleGetGlobal_v2",real_global,hook_global),HOOK("cuFuncGetAttribute",real_attribute,hook_attribute),
    HOOK("cuGetExportTable",real_export,hook_export),
    HOOK("cuLaunchKernelEx",real_launch_ex,hook_launch_ex),HOOK("cuLaunchKernelEx_ptsz",real_launch_ex_ptsz,hook_launch_ex_ptsz),
    HOOK("cuLaunchCooperativeKernel",real_cooperative,hook_cooperative),HOOK("cuLaunchCooperativeKernel_ptsz",real_cooperative_ptsz,hook_cooperative_ptsz),
    HOOK("cuLibraryLoadData",real_library_data,hook_library_data),HOOK("cuLibraryLoadFromFile",real_library_file,hook_library_file),
    HOOK("cuMemFree_v2",real_free,hook_free),HOOK("cuCtxDestroy_v2",real_destroy,hook_destroy),
    HOOK("cuDevicePrimaryCtxRelease_v2",real_release,hook_release),HOOK("cuDevicePrimaryCtxReset_v2",real_reset,hook_reset),
    HOOK("cuMemcpyHtoD_v2",real_htod,hook_htod),HOOK("cuMemcpyHtoDAsync_v2",real_htod_async,hook_htod_async),
    HOOK("cuMemcpyDtoD_v2",real_dtod,hook_dtod),HOOK("cuMemcpyDtoDAsync_v2",real_dtod_async,hook_dtod_async),
    HOOK("cuMemcpyHtoDAsync_v2_ptsz",real_htod_async_ptsz,hook_htod_async_ptsz),
    HOOK("cuMemcpyDtoDAsync_v2_ptsz",real_dtod_async_ptsz,hook_dtod_async_ptsz),
    HOOK("cuMemcpy",real_copy,hook_copy),HOOK("cuMemcpyAsync",real_copy_async,hook_copy_async),
    HOOK("cuMemcpyAsync_ptsz",real_copy_async_ptsz,hook_copy_async_ptsz),
    HOOK("cuMemcpyHtoD_v2_ptds",real_htod_ptds,hook_htod_ptds),HOOK("cuMemcpyDtoD_v2_ptds",real_dtod_ptds,hook_dtod_ptds),
    HOOK("cuMemcpy_ptds",real_copy_ptds,hook_copy_ptds),
    HOOK("cuGraphAddKernelNode_v2",real_graph_kernel,hook_graph_kernel),
#define SET_HOOKS(bits) \
    HOOK("cuMemsetD" #bits "_v2",real_set##bits,hook_set##bits), \
    HOOK("cuMemsetD" #bits "_v2_ptds",real_set##bits##_ptds,hook_set##bits##_ptds), \
    HOOK("cuMemsetD" #bits "Async",real_set##bits##_async,hook_set##bits##_async), \
    HOOK("cuMemsetD" #bits "Async_ptsz",real_set##bits##_async_ptsz,hook_set##bits##_async_ptsz),
    SET_HOOKS(8)
    SET_HOOKS(16)
    SET_HOOKS(32)
};
// Driver lookups may return versioned/private entry addresses, not the exported
// address patched by Detours. Route supported names explicitly after native validation.
void* resolve(const std::string& name,cuuint64_t flags) {
    if(name=="cuStreamDestroy")return reinterpret_cast<void*>(hook_stream_destroy);
    if(name=="cuStreamQuery")return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_stream_query_ptsz:hook_stream_query);
    if(name=="cuStreamSynchronize")return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_stream_sync_ptsz:hook_stream_sync);
    if(name=="cuMemcpyDtoH")return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_dtoh_ptds:hook_dtoh);
    if(name=="cuLaunchKernel") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_launch_ptsz:hook_launch);
    if(name=="cuModuleGetGlobal") return reinterpret_cast<void*>(hook_global);
    if(name=="cuMemFree") return reinterpret_cast<void*>(hook_free);
    if(name=="cuCtxDestroy") return reinterpret_cast<void*>(hook_destroy);
    if(name=="cuDevicePrimaryCtxRelease") return reinterpret_cast<void*>(hook_release);
    if(name=="cuDevicePrimaryCtxReset") return reinterpret_cast<void*>(hook_reset);
    if(name=="cuMemcpyHtoD") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_htod_ptds:hook_htod);
    if(name=="cuMemcpyHtoDAsync") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_htod_async_ptsz:hook_htod_async);
    if(name=="cuMemcpyDtoD") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_dtod_ptds:hook_dtod);
    if(name=="cuMemcpy") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_copy_ptds:hook_copy);
    if(name=="cuMemcpyDtoDAsync") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_dtod_async_ptsz:hook_dtod_async);
    if(name=="cuMemcpyAsync") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_copy_async_ptsz:hook_copy_async);
    if(name=="cuGraphAddKernelNode") return reinterpret_cast<void*>(hook_graph_kernel);
#define RESOLVE_SET(bits) \
    if(name=="cuMemsetD" #bits) return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_set##bits##_ptds:hook_set##bits); \
    if(name=="cuMemsetD" #bits "Async") return reinterpret_cast<void*>(flags==CU_GET_PROC_ADDRESS_PER_THREAD_DEFAULT_STREAM?hook_set##bits##_async_ptsz:hook_set##bits##_async);
    RESOLVE_SET(8)
    RESOLVE_SET(16)
    RESOLVE_SET(32)
    for(const auto& h:hooks) if(name==h.name) return h.replacement;
    return nullptr;
}
bool legacy_memory_abi(const std::string& name,int version) {
    return version<3020 && (name=="cuMemFree" || name=="cuModuleGetGlobal" ||
        name=="cuMemcpyDtoH" || name=="cuMemcpyHtoD" || name=="cuMemcpyHtoDAsync" || name=="cuMemcpyDtoD" || name=="cuMemcpyDtoDAsync" ||
        name=="cuMemsetD8" || name=="cuMemsetD16" || name=="cuMemsetD32");
}
CUresult CUDAAPI hook_proc1(const char* symbol,void** out,int version,cuuint64_t flags) {
    if(internal)return real_proc1(symbol,out,version,flags);
    return invoke([&]{
        void* native=nullptr; const auto result=real_proc1(symbol,&native,version,flags); if(result!=CUDA_SUCCESS)return result;
        const auto name=text(symbol,256); void* replacement=resolve(name,flags);
        // Old ABI variants may have different argument sizes; do not reinterpret them.
        // cuda.lib probes legacy variants during initialization; a missing capability
        // is normal negotiation, not a rejected application operation.
        if(replacement && legacy_memory_abi(name,version)){publish(out,static_cast<void*>(nullptr));return CUDA_ERROR_NOT_SUPPORTED;}
        if(name=="cuGetProcAddress") replacement=version>=12000?reinterpret_cast<void*>(hook_proc2):reinterpret_cast<void*>(hook_proc1);
        publish(out,replacement?replacement:native); return CUDA_SUCCESS;
    });
}
CUresult CUDAAPI hook_proc2(const char* symbol,void** out,int version,cuuint64_t flags,CUdriverProcAddressQueryResult* status) {
    if(internal)return real_proc2(symbol,out,version,flags,status);
    return invoke([&]{
        void* native=nullptr; CUdriverProcAddressQueryResult query=CU_GET_PROC_ADDRESS_SYMBOL_NOT_FOUND;
        const auto result=real_proc2(symbol,&native,version,flags,&query); if(status)publish(status,query);
        if(result!=CUDA_SUCCESS)return result;
        if(query!=CU_GET_PROC_ADDRESS_SUCCESS || !native){publish(out,native);return result;}
        const auto name=text(symbol,256); void* replacement=resolve(name,flags);
        if(replacement && legacy_memory_abi(name,version)){
            publish(out,static_cast<void*>(nullptr));if(status)publish(status,CU_GET_PROC_ADDRESS_VERSION_NOT_SUFFICIENT);
            return CUDA_ERROR_NOT_SUPPORTED;
        }
        if(name=="cuGetProcAddress") replacement=version>=12000?reinterpret_cast<void*>(hook_proc2):reinterpret_cast<void*>(hook_proc1);
        publish(out,replacement?replacement:native); return CUDA_SUCCESS;
    });
}
bool install() {
    const auto driver=GetModuleHandleW(L"nvcuda.dll"); if(!driver)return false;
    HMODULE pinned=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,L"nvcuda.dll",&pinned))return false;
    if(DetourTransactionBegin()!=NO_ERROR)return false;
    if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR){DetourTransactionAbort();return false;}
    for(auto& h:hooks) {
        *h.real=reinterpret_cast<void*>(GetProcAddress(driver,h.name));
        const auto error=*h.real?DetourAttach(h.real,h.replacement):ERROR_PROC_NOT_FOUND;
        if(error!=NO_ERROR){std::fprintf(stderr,"[HBFLab inject] cannot hook %s: %ld\n",h.name,error);DetourTransactionAbort();return false;}
    }
    const auto error=DetourTransactionCommit();
    if(error!=NO_ERROR)std::fprintf(stderr,"[HBFLab inject] hook transaction failed: %ld\n",error);
    return error==NO_ERROR;
}
// CUDA 13's cuda.lib loads nvcuda lazily. Intercept loader completion so no CUDA
// initialization is needed in DllMain, and the first resolved Driver call is covered.
decltype(&LoadLibraryA) real_library_a=LoadLibraryA;
decltype(&LoadLibraryW) real_library_w=LoadLibraryW;
decltype(&LoadLibraryExA) real_library_exa=LoadLibraryExA;
decltype(&LoadLibraryExW) real_library_exw=LoadLibraryExW;
SRWLOCK install_lock=SRWLOCK_INIT;
bool installed=false;
void driver_loaded() {
    AcquireSRWLockExclusive(&install_lock);
    if(!installed && GetModuleHandleW(L"nvcuda.dll")) {
        installed=install();
        // Continuing after partial/failed instrumentation could silently bypass timing.
        if(!installed)TerminateProcess(GetCurrentProcess(),ERROR_DLL_INIT_FAILED);
        driver_ready.store(true,std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&install_lock);
}
HMODULE WINAPI library_a(LPCSTR n){const auto m=real_library_a(n);const auto e=GetLastError();if(m)driver_loaded();SetLastError(e);return m;}
HMODULE WINAPI library_w(LPCWSTR n){const auto m=real_library_w(n);const auto e=GetLastError();if(m)driver_loaded();SetLastError(e);return m;}
HMODULE WINAPI library_exa(LPCSTR n,HANDLE f,DWORD flags){const auto m=real_library_exa(n,f,flags);const auto e=GetLastError();if(m)driver_loaded();SetLastError(e);return m;}
HMODULE WINAPI library_exw(LPCWSTR n,HANDLE f,DWORD flags){const auto m=real_library_exw(n,f,flags);const auto e=GetLastError();if(m)driver_loaded();SetLastError(e);return m;}
bool bootstrap() {
    if(DetourTransactionBegin()!=NO_ERROR)return false;
    if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR){DetourTransactionAbort();return false;}
    Hook loaders[]={HOOK("LoadLibraryA",real_library_a,library_a),HOOK("LoadLibraryW",real_library_w,library_w),
        HOOK("LoadLibraryExA",real_library_exa,library_exa),HOOK("LoadLibraryExW",real_library_exw,library_exw)};
    for(auto& h:loaders)if(DetourAttach(h.real,h.replacement)!=NO_ERROR){DetourTransactionAbort();return false;}
    return DetourTransactionCommit()==NO_ERROR;
}
} // namespace

extern "C" __declspec(dllexport) int WINAPI HBFLabInjectionQuery(HBFLabInjectionStats* out,unsigned bytes) {
    if(!out || bytes!=sizeof(*out))return 0;
    Guard guard; auto snapshot=counters; snapshot.active=driver_ready.load(std::memory_order_acquire)?1:0;
    snapshot.live_modules=session?session->modules.size():0;
    SIZE_T written=0;
    return WriteProcessMemory(GetCurrentProcess(),out,&snapshot,sizeof(snapshot),&written) && written==sizeof(snapshot);
}
extern "C" __declspec(dllexport) int WINAPI HBFLabInjectionAsyncQuery(HBFLabInjectionAsyncStats* out,unsigned bytes) {
    if(!out || bytes!=sizeof(*out))return 0;
    Guard guard;
    try {
        HBFLabInjectionAsyncStats value{1,0,0,0,0};
        if(session && session->service){session->current();const auto s=session->service->statistics();value={1,1,s.launches_completed,s.completed,s.peak_active_launches};}
        publish(out,value);return 1;
    }catch(...){return 0;}
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(DetourIsHelperProcess())return TRUE;
    if(reason==DLL_PROCESS_ATTACH) {
        DetourRestoreAfterWith(); DisableThreadLibraryCalls(instance);
        char enabled[2]{};
        if(GetEnvironmentVariableA("HBFLAB_INJECT_ACTIVE",enabled,2)!=1 || enabled[0]!='1')return FALSE;
        HMODULE pinned=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&DllMain),&pinned))return FALSE;
        if(!bootstrap())return FALSE;
        driver_loaded();
    }
    // Process exit: no CUDA calls or C++ Runtime destruction under loader lock.
    return TRUE;
}
