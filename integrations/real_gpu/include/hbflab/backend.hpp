#pragma once
#include "hbflab/protocol.hpp"
#include "hbf_eventlab/runtime/simulation_session.hpp"
#include <memory>
#include <array>
#include <limits>
#include <vector>
#include <optional>

namespace hbflab {
struct BackendOptions {
    hbf_eventlab::Geometry geometry{1, 1, 2, 16, 64, 1};
    hbf_eventlab::ExecutionConfiguration timing{};
    std::uint32_t outstanding = 256;
    std::size_t resident_fragments = 256; // Host active logical 64B fragments, at least one full DLU
    std::uint64_t max_events_per_batch = 1000000;
    bool fail_program = false; // deterministic fault input for contract tests
    std::uint64_t fail_program_after = (std::numeric_limits<std::uint64_t>::max)();
};
struct ReadResult {
    Status status = Status::BackendError;
    std::uint64_t begin_ps = 0, end_ps = 0;
    std::uint32_t sectors = 0;
};
struct WriteResult {
    Status status = Status::BackendError;
    std::uint64_t begin_ps = 0, end_ps = 0;
    std::uint32_t replayed_pages = 0;
};
struct ReadSpan { std::uint64_t logical; std::uint32_t bytes; };
struct TimedRead {
    std::uint64_t arrival_ps;
    std::vector<ReadSpan> spans; // empty contribution closes a producer's frontier
};
struct StreamReadResult { std::uint64_t id; ReadResult result; };
struct StreamWriteResult { std::uint64_t id; WriteResult result; };
struct BackendStats {
    std::uint64_t requests = 0, sectors = 0, backpressure = 0;
    std::uint64_t writebacks = 0, replayed_pages = 0;
    std::uint64_t dependency_waits = 0;
    std::size_t peak_registered_globals = 0;
    std::size_t peak_resident_fragments = 0;
    std::uint64_t registered_globals = 0, completed_globals = 0, registered_fragments = 0;
    bool input_closed = false;
    hbf_eventlab::HostStatistics host{};
    hbf_eventlab::DeviceStatistics device{};
};
struct MediaSample {std::uint64_t time_ps;BackendStats stats;};
// Borrowed transport: the owner serializes access to one device and clock.
// Capacity keeps real bytes and leases; this interface only models media work.
class CapacityMedia {
public:
    virtual ~CapacityMedia()=default;
    virtual std::uint64_t reserve_media(std::uint64_t)=0;
    virtual ReadResult read_media(std::uint64_t,std::uint32_t)=0;
    virtual WriteResult write_media(std::uint64_t,const std::array<std::uint64_t,64>&)=0;
    virtual BackendStats media_statistics()=0;
    virtual std::uint64_t media_time_ps()=0;
    virtual void abort_media()=0;
    // Threaded owners override this to capture clock and counters atomically.
    virtual MediaSample sample_media(){const auto stats=media_statistics();return {media_time_ps(),stats};}
    virtual std::optional<hbf_eventlab::Geometry> media_geometry(){return std::nullopt;}
};
// Single host thread owns this object. Media/timing state survives each GPU access.
// Byte authority remains in the immutable CUDA allocation, never in DataToken.
class TimingBackend {
public:
    explicit TimingBackend(BackendOptions = {});
    ~TimingBackend();
    TimingBackend(const TimingBackend&) = delete;
    TimingBackend& operator=(const TimingBackend&) = delete;
    std::uint64_t reserve(std::uint64_t page_aligned_bytes);
    ReadResult read(std::uint64_t logical, std::uint32_t bytes, std::uint64_t compute_gap_ps = 0);
    // Sorted by arrival with stable ties. Parents share the Host resident budget.
    std::vector<ReadResult> read_batch(const std::vector<TimedRead>&);
    // Incremental read session. Submit only strictly future arrivals, sealed
    // in sorted order before advancing. No event at or beyond the exclusive
    // watermark is executed; UINT64_MAX is reserved as the unbounded sentinel.
    void begin_stream(std::size_t parent_limit);
    void submit_read(std::uint64_t id, const TimedRead&);
    // Shares IDs, ordered arrivals, parent bound and Host admission with reads.
    // A logical block snapshot excludes conflicting producer reads and writes.
    void submit_write(std::uint64_t id,std::uint64_t arrival_ps,std::uint64_t logical,
                      const std::array<std::uint64_t,64>&);
    bool advance_before(std::uint64_t exclusive_ps);
    std::vector<StreamReadResult> take_completed();
    std::vector<StreamWriteResult> take_completed_writes();
    void end_stream(); // requires all results consumed and the device drained
    void finish(); // final close; requires no producer work and forbids reuse
    // Full-page writeback creates one new immutable logical block snapshot.
    // Physical splitting, retry and replay belong to Host; real bytes stay in CapacityStore.
    WriteResult write_page(std::uint64_t logical, const std::array<std::uint64_t,64>& tokens);
    std::uint64_t now_ps() const;
    BackendStats statistics() const;
    bool drained() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
