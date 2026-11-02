#pragma once
#include "hbflab/backend.hpp"
#include "hbflab/async_protocol.hpp"
#include <cuda_runtime_api.h>
#include <memory>
namespace hbflab {
struct AsyncLaunchShape {std::uint32_t blocks=1,threads=1;};
struct AsyncRuntimeOptions {
    BackendOptions backend{};
    std::uint32_t time_scale=1;
    std::uint64_t timeout_ns=500000000;
    std::uint64_t service_timeout_ns=10000000000ull;
    std::size_t audit_limit=4096;
};
struct AsyncServiceTrace {
    std::uint64_t launch=0,ticket=0,arrival_ns=0,arrival_ps=0,end_ps=0;
    std::uint32_t producer=0,sectors=0;
    std::uint64_t dispatch=0,grant_floor_ns=0;
    std::uint64_t first_address=0;
    std::uint32_t first_bytes=0,span_count=0;
};
struct AsyncRuntimeStats {
    std::uint64_t grants=0,submitted=0,completed=0,audit_dropped=0;
    std::uint64_t launches_submitted=0,launches_completed=0;
    std::uint64_t gpu_epoch_ns=0,model_epoch_ps=0,model_now_ps=0;
    std::size_t peak_inflight=0;
    std::size_t peak_active_launches=0;
    BackendStats backend{};
    std::uint64_t media_submitted=0,media_completed=0;
    std::size_t peak_gpu_during_media=0;
};
struct AsyncMediaTrace {
    std::uint64_t ticket=0,dispatch=0,arrival_ns=0,arrival_ps=0,end_ps=0,floor_ns=0,logical=0;
    std::uint32_t producer=0,bytes=0,replayed_pages=0;
    bool write=false;
    std::uint64_t ready_wall_ns=0,completion_wall_ns=0,resumed_wall_ns=0,delay_ns=0;
};
// One owner host thread/context; multiple GPU streams/kernels/CTAs.
// Batch sessions predeclare all launches; live sessions reuse bounded slots.
// Both keep registered ranges frozen until completion.
class AsyncRuntime {
public:
    explicit AsyncRuntime(AsyncRuntimeOptions={});
    ~AsyncRuntime();
    AsyncRuntime(const AsyncRuntime&)=delete;
    AsyncRuntime& operator=(const AsyncRuntime&)=delete;
    const void* upload(const void* source,std::size_t bytes);
    // Borrow immutable framework memory until the session has finished.
    void register_readonly(const void* source,std::size_t bytes);
    std::uint64_t reserve_media(std::uint64_t bytes);
    ReadResult read_media(std::uint64_t logical,std::uint32_t bytes);
    WriteResult write_media(std::uint64_t logical,const std::array<std::uint64_t,64>&);
    std::vector<AsyncMediaTrace> take_media_audit();
    void abort();
    void prepare(const std::vector<AsyncLaunchShape>&);
    // Kernel first parameter MUST be AsyncDeviceView by value. args contains
    // pointers to the remaining arguments. Copies arguments before returning.
    // Each prepared index may be launched exactly once, on its chosen stream.
    void launch(std::uint32_t index,const void* kernel,const std::vector<void*>& args,cudaStream_t stream);
    Status serve();
    // Persistent alternative to prepare/launch/serve. The same owner pumps all
    // launches; unused slots cannot timestamp requests. Ranges stay frozen.
    void start_live(std::uint32_t producers,std::uint32_t launch_limit=16);
    // Returns 0 on bounded pool pressure; retry after poll_live(). Arguments are
    // copied by CUDA before return. The first kernel argument is AsyncDeviceView.
    std::uint64_t launch_live(AsyncLaunchShape,const void* kernel,const std::vector<void*>& args,
                              cudaStream_t stream,std::size_t shared_bytes=0);
    std::uint64_t launch_driver_live(AsyncLaunchShape,std::uintptr_t function,const std::vector<void*>& args,cudaStream_t);
    Status poll_live();
    bool live_idle() const;
    void finish_live();
    AsyncRuntimeStats statistics() const;
    std::vector<AsyncServiceTrace> audit() const;
    // Transfer completed audit records without resetting counters or epochs.
    std::vector<AsyncServiceTrace> take_audit();
    bool drained() const;
private:
    std::uint64_t launch_live_impl(AsyncLaunchShape,const void*,const std::vector<void*>&,cudaStream_t,std::size_t,bool);
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
