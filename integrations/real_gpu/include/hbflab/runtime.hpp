#pragma once
#include "hbflab/backend.hpp"
#include <cuda_runtime_api.h>
#include <memory>

namespace hbflab {
struct RuntimeOptions {
    BackendOptions backend{};
    std::uint32_t time_scale = 1;
    std::uint64_t timeout_ns = 500000000; // less than typical display watchdog limits
    std::uint32_t threads = 1;
    ProducerMode mode = ProducerMode::Thread;
};
struct ServiceTrace {
    std::uint32_t producer = 0, spans = 0, sectors = 0;
    std::uint64_t ticket = 0, arrival_ns = 0, arrival_ps = 0, end_ps = 0;
};
class Runtime {
public:
    explicit Runtime(RuntimeOptions = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    // Upload immutable bytes before view(). Caller may not write/free the returned allocation.
    const std::uint32_t* upload(const void* source, std::size_t bytes);
    // Borrow a CUDA device interval without changing its pointer or owning/freeing it.
    // Caller keeps it allocated and immutable until this Runtime is destroyed.
    void register_readonly(const void* device_source, std::size_t bytes);
    // Freezes registration. One active CTA, exactly options.threads physical threads.
    DeviceView view();
    // Launch first on this stream, then call immediately. CPU serves requests while
    // that SAME kernel is resident. Returns terminal status after the stream drains.
    Status serve(cudaStream_t stream);
    BackendStats statistics() const;
    // Bounded audit of the last closed frontier, in deterministic dispatch order.
    std::vector<ServiceTrace> last_frontier() const;
    bool drained() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
