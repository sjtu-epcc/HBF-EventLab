#pragma once
#include "hbflab/capacity.hpp"
#include <cuda_runtime_api.h>
namespace hbflab {
struct GpuPageLease {std::uint64_t owner=0,value=0;};
struct CapacityGpuStats {
    std::uint64_t uploads=0,downloads=0,mirror_hits=0,h2d_bytes=0,d2h_bytes=0,copy_wall_ns=0;
    std::uint64_t prefetch_uploads=0;
    std::size_t device_bytes=0,active_leases=0;
};
// Explicit kernel-boundary paging: pointer valid only until finish(), all work
// must use the stream passed to acquire(). No device-side demand-fault protocol.
class CapacityGpu {
public:
    explicit CapacityGpu(CapacityOptions = {},CapacityMedia* shared_media=nullptr);
    ~CapacityGpu();
    CapacityGpu(const CapacityGpu&)=delete;
    CapacityGpu& operator=(const CapacityGpu&)=delete;
    CapacityStore& store();
    GpuPageLease acquire(Allocation,std::size_t page,PageAccess,cudaStream_t=nullptr);
    const void* data(GpuPageLease) const;
    void* writable_data(GpuPageLease) const;
    std::size_t bytes(GpuPageLease) const;
    void finish(GpuPageLease); // Drain stream, collect writable bytes, then unpin.
    void flush(); // All leases must be finished. Wait for modeled writeback duration.
    std::size_t prefetch(Allocation,std::size_t first_page,std::size_t count);
    std::size_t maintain(double gpu_watts=0);
    std::size_t device_bytes() const;
    CapacityGpuStats statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
