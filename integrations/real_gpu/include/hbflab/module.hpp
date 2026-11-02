#pragma once
#include "hbflab/runtime.hpp"
#include "hbflab/ptx.hpp"
#include <filesystem>

namespace hbflab {
struct ModuleHandle { std::uint64_t value = 0; };
struct FunctionHandle { std::uint64_t value = 0; };
struct OutputSpan { void* data = nullptr; std::size_t bytes = 0; };
struct ModuleOptions {
    std::size_t cache_entries = 8, cache_bytes = 16 * 1024 * 1024;
};
struct ModuleStats {
    std::uint64_t jit_compilations = 0, cache_hits = 0, cache_evictions = 0;
    std::size_t cache_entries = 0, cache_bytes = 0, live_modules = 0, live_functions = 0;
};
struct ModuleIdentity {
    std::string source_sha256, rewritten_sha256, helper_sha256, artifact_sha256, cubin_sha256;
    std::uint64_t instance = 0;
    std::uint64_t context_id = 0;
    unsigned frontend = ptx_frontend_version, protocol = abi_version;
    int device = 0, target_sm = 0, driver_version = 0;
    std::vector<PtxEntry> entries;
};
// Explicit Driver-API interception boundary. No raw CUmodule/CUfunction escapes.
// Runtime must outlive this object; use the creating host thread and CUDA context.
class ModuleInterceptor {
public:
    explicit ModuleInterceptor(Runtime&, ModuleOptions = {});
    ~ModuleInterceptor();
    ModuleInterceptor(const ModuleInterceptor&) = delete;
    ModuleInterceptor& operator=(const ModuleInterceptor&) = delete;
    ModuleHandle load_ptx(std::string_view);
    ModuleHandle load_file(const std::filesystem::path&);
    FunctionHandle function(ModuleHandle, const std::string& name);
    ModuleIdentity identity(ModuleHandle) const;
    ModuleStats statistics() const;
    // Releases cached binaries only; loaded module instances and handles stay valid.
    void clear_cache();
    void unload(ModuleHandle);
    // Synchronous launch + Runtime::serve. All dimensions and argument counts validated.
    // Output stores may only target this bounded allocation, never an uploaded range.
    Status launch(FunctionHandle, dim3 grid, dim3 block, const std::vector<void*>& args,
                  OutputSpan output = {}, cudaStream_t stream = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
