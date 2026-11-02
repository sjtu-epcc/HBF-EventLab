#pragma once
#include "hbflab/backend.hpp"
#include "hbflab/environment.hpp"
#include <filesystem>
#include <stdexcept>

namespace hbflab {
constexpr std::size_t capacity_page_bytes=4096;
enum class BackingMode { Memory, CreateFile, OpenFile };
struct CapacityOptions {
    std::size_t logical_pages=64, frames=8, max_pins=256;
    BackendOptions backend{};
    BackingMode backing_mode=BackingMode::Memory;
    std::filesystem::path backing_path;
    std::size_t read_ahead_pages=0, prefetch_budget=4;
    bool prefetch_evict_clean=false; // Explicit opt-in to cache pollution risk.
    EnvironmentOptions environment{};
};
struct Allocation { std::uint64_t store=0,value=0; };
struct PageLease { std::uint64_t store=0,value=0; };
enum class PageAccess { Read, Write };
struct PageInfo {
    std::size_t frame=0, page=0, bytes=0;
    std::uint64_t generation=0,revision=0;
    PageAccess access=PageAccess::Read;
};
struct CapacityStats {
    std::uint64_t hits=0, faults=0, evictions=0, writebacks=0, discarded_dirty=0;
    std::size_t allocated_pages=0, resident_frames=0, dirty_frames=0, pins=0, peak_pins=0;
    BackendStats backend{};
    bool poisoned=false;
    std::uint64_t file_read_bytes=0,file_write_bytes=0,checkpoints=0;
    std::uint64_t prefetch_fills=0,prefetch_useful=0,prefetch_wasted=0,prefetch_skipped=0;
    std::uint64_t refreshes=0,refresh_read_bytes=0,retention_expirations=0;
    std::size_t refresh_pending=0,refresh_blocked=0;
    EnvironmentStats environment{};
};
class CapacityBusy:public std::runtime_error { public:using std::runtime_error::runtime_error; };
// Single host owner. Backing bytes become authoritative only after successful
// EventLab writeback. A dirty pinned/resident frame owns the latest bytes.
class CapacityStore {
public:
    // Optional borrowed shared-device port must outlive this store. Reserve
    // before its live session starts; all later I/O goes through that owner.
    explicit CapacityStore(CapacityOptions = {},CapacityMedia* shared_media=nullptr);
    ~CapacityStore(); // Does not silently flush dirty data.
    CapacityStore(const CapacityStore&)=delete;
    CapacityStore& operator=(const CapacityStore&)=delete;
    Allocation allocate(std::size_t bytes);
    void release(Allocation); // Explicit discard; requires no leases on allocation.
    std::size_t size(Allocation) const;
    std::size_t free_pages() const;
    std::size_t frame_count() const;
    // Reopened stores receive a new store identity; discover restored objects here.
    std::vector<Allocation> allocations() const;
    std::size_t pin_count(Allocation) const;
    PageLease pin(Allocation,std::size_t page_index,PageAccess=PageAccess::Read);
    PageInfo info(PageLease) const;
    void read_pinned(PageLease,std::size_t offset,void* destination,std::size_t bytes) const;
    void write_pinned(PageLease,std::size_t offset,const void* source,std::size_t bytes);
    void unpin(PageLease);
    void read(Allocation,std::size_t offset,void* destination,std::size_t bytes);
    void write(Allocation,std::size_t offset,const void* source,std::size_t bytes);
    void flush(Allocation);
    void flush_all();
    // Synchronous, bounded lookahead. Never evicts dirty/pinned frames; demand
    // eviction requires prefetch_evict_clean. Speculative frames are replaceable.
    std::size_t prefetch(Allocation,std::size_t first_page,std::size_t count);
    // Explicit maintenance service point, on the existing media owner/clock.
    // Returns completed logical refreshes; leased pages are deferred, never stolen.
    std::size_t maintain(double gpu_watts=0);
    CapacityStats statistics() const;
    std::uint64_t model_time_ps() const;
    // Bridge reports irrecoverable CUDA/transport failure; no later data access.
    void abort();
private:
    friend class CapacityGpu;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PageLease pin_impl(Allocation,std::size_t,PageAccess,bool speculative);
    std::optional<PageLease> pin_prefetched(Allocation,std::size_t);
};
}
