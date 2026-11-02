#pragma once
#include "hbflab/async_runtime.hpp"

namespace hbflab {
// Optional trusted embedding hooks. Executed only by the service worker, before
// its first CUDA call and after its last; callbacks must not throw.
struct LiveWorkerHooks {void (*enter)() noexcept=nullptr;void (*leave)() noexcept=nullptr;};
// Framework thread owns the service handle and borrowed data lifetime. One
// dedicated context-bound worker exclusively owns AsyncRuntime and EventLab.
class LiveTimingService:public CapacityMedia {
public:
    explicit LiveTimingService(AsyncRuntimeOptions={},std::uint32_t producers=4096,std::uint32_t launch_limit=4,LiveWorkerHooks={});
    ~LiveTimingService();
    LiveTimingService(const LiveTimingService&)=delete;
    LiveTimingService& operator=(const LiveTimingService&)=delete;
    void register_readonly(const void*,std::size_t);
    void start();
    std::uint64_t launch(AsyncLaunchShape,const void*,const std::vector<void*>&,cudaStream_t,std::size_t shared_bytes=0);
    std::uint64_t launch_driver(AsyncLaunchShape,std::uintptr_t,const std::vector<void*>&,cudaStream_t);
    void synchronize();
    void check_status(); // One service poll, no wait for all launches.
    void finish();
    AsyncRuntimeStats statistics();
    std::vector<AsyncServiceTrace> audit();
    std::vector<AsyncServiceTrace> take_audit();
    bool drained();
    std::uint64_t reserve_media(std::uint64_t) override;
    ReadResult read_media(std::uint64_t,std::uint32_t) override;
    WriteResult write_media(std::uint64_t,const std::array<std::uint64_t,64>&) override;
    BackendStats media_statistics() override;
    std::uint64_t media_time_ps() override;
    void abort_media() override;
    MediaSample sample_media() override;
    std::optional<hbf_eventlab::Geometry> media_geometry() override;
    std::vector<AsyncMediaTrace> take_media_audit();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
