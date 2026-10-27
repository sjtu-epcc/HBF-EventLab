#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace hbf_eventlab {
// Program wall time only. Never used to advance simulation time or make decisions.
enum class RuntimeRegion : std::size_t {
    Split, Prepare, RegisterInput, AdmitInput, Pump, Receive, Advance,
    ResultsScan, Reclaim, EventRun, Count
};
enum class RuntimeCounter : std::size_t {
    SplitFragments, PrepareGlobals, RegisteredGlobals, RegisteredFragments,
    InputRegistered, InputBackpressured, InputTooLarge, BackpressuredFragments,
    PumpQueueVisits, PumpCandidates, HandoffAttempts, HandoffTaken,
    ResultsEntriesVisited, ResultsSizePeak, ReclaimUnitVisits,
    ReclaimedWork, ReclaimedUnits, ReclaimedRequests, Count
};
struct RuntimeMeasurement {
    std::uint64_t calls=0, unwound_calls=0, inclusive_ns=0, self_ns=0, max_ns=0;
};
class RuntimeScope;
// Single simulation thread, per instance, fixed-size aggregates; no per-call log.
// The owner must outlive all attached Hosts/Sessions. Pass nullptr for the default
// fast path: no clock reads, counter updates, output, or allocations.
class RuntimeProfile {
public:
    // Optional deterministic test clock: monotonic nanoseconds, must not throw.
    using Clock=std::uint64_t(*)() noexcept;
    explicit RuntimeProfile(Clock clock=nullptr) noexcept;
    RuntimeProfile(const RuntimeProfile&)=delete;
    RuntimeProfile& operator=(const RuntimeProfile&)=delete;
    const RuntimeMeasurement& measurement(RuntimeRegion r) const noexcept { return regions_[static_cast<std::size_t>(r)]; }
    std::uint64_t counter(RuntimeCounter c) const noexcept { return counters_[static_cast<std::size_t>(c)]; }
    void add(RuntimeCounter c,std::uint64_t n=1) noexcept { counters_[static_cast<std::size_t>(c)]+=n; }
    void peak(RuntimeCounter c,std::uint64_t n) noexcept {
        auto& value=counters_[static_cast<std::size_t>(c)];if(n>value)value=n;
    }
    std::uint64_t covered_root_ns() const noexcept { return root_ns_; }
    // Export only at a quiescent API boundary; caller owns the stream/file.
    // self excludes instrumented children only. Inclusive totals must not be summed.
    void write_json(std::ostream&) const;
private:
    friend class RuntimeScope;
    Clock clock_;
    RuntimeScope* active_=nullptr;
    std::array<RuntimeMeasurement,static_cast<std::size_t>(RuntimeRegion::Count)> regions_{};
    std::array<std::uint64_t,static_cast<std::size_t>(RuntimeCounter::Count)> counters_{};
    std::uint64_t root_ns_=0;
};
class RuntimeScope {
public:
    RuntimeScope(RuntimeProfile* profile,RuntimeRegion region) noexcept;
    ~RuntimeScope() noexcept;
    RuntimeScope(const RuntimeScope&)=delete;
    RuntimeScope& operator=(const RuntimeScope&)=delete;
private:
    friend class RuntimeProfile;
    RuntimeProfile* profile_;
    RuntimeRegion region_;
    RuntimeScope* parent_=nullptr;
    std::uint64_t start_=0, children_=0;
    int exceptions_=0;
};
} // namespace hbf_eventlab
