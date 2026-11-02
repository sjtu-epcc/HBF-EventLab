#pragma once
#include "hbflab/module.hpp"
#include "hbflab/live_runtime.hpp"
namespace hbflab {
// Service outlives loader. Per-launch hidden parameters; no mutable module globals.
struct AsyncModuleHandle {std::uint64_t value=0;};
struct AsyncFunctionHandle {std::uint64_t value=0;};
struct NativeReadSpan {const void* data=nullptr;std::size_t bytes=0;};
class AsyncModuleInterceptor {
public:
    explicit AsyncModuleInterceptor(LiveTimingService&,ModuleOptions={});
    ~AsyncModuleInterceptor();
    AsyncModuleInterceptor(const AsyncModuleInterceptor&)=delete;
    AsyncModuleInterceptor& operator=(const AsyncModuleInterceptor&)=delete;
    AsyncModuleHandle load_ptx(std::string_view);
    AsyncModuleHandle load_file(const std::filesystem::path&);
    AsyncFunctionHandle function(AsyncModuleHandle,const std::string&);
    ModuleIdentity identity(AsyncModuleHandle) const;
    ModuleStats statistics() const;
    void clear_cache();
    void unload(AsyncModuleHandle); // Drains service before releasing executable code.
    std::uint64_t launch(AsyncFunctionHandle,AsyncLaunchShape,const std::vector<void*>&,OutputSpan={},cudaStream_t=nullptr,NativeReadSpan={});
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
