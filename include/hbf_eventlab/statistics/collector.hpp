#pragma once
#include "hbf_eventlab/interfaces/statistics_sink.hpp"
#include <iosfwd>
#include <map>

namespace hbf_eventlab {
struct GlobalTiming {
    GlobalRequestId id; Op op; std::uint64_t bytes; Tick scheduled_release;
    std::optional<Tick> released,finished;
    LogicalResult result=LogicalResult::Pending;
    std::size_t children=0,remaining=0,failed=0;
    std::optional<Tick> last_child_finished;
    std::string reason;
};
struct LogicalTiming {
    LogicalWorkId id; GlobalRequestId parent; ChannelAddress address;
    std::optional<std::uint64_t> content;
    std::optional<Tick> finished;
    LogicalResult result=LogicalResult::Pending;
    bool cancelled_before_release=false;
    std::string reason;
};
struct AttemptTiming {
    TransferAttemptId id; HostCommand command; Tick handoff;
    std::optional<Tick> rx_arrival,accepted,delivered,assessed;
    std::optional<std::uint8_t> status;
    std::optional<bool> host_success;
    std::optional<SendUnitId> unit;
    std::optional<Tick> queued_at;
    std::uint64_t unit_attempt=0,replay_pass=0;
    bool replay=false;
    std::vector<LogicalWorkId> work;
};
struct ResultCounts {
    std::uint64_t total=0,success=0,failed=0,pending=0,cancelled_before_release=0;
};
struct LatencySummary {
    std::uint64_t count=0,min=0,max=0,p50=0,p95=0,p99=0;
    long double mean=0;
};
struct StatisticsSummary {
    Tick at;
    std::map<Op,ResultCounts> globals,logical_work,attempts;
    // Keys include read/write, level, and outcome. No mixed-status latency pool.
    std::map<std::string,LatencySummary> latency;
    std::map<std::pair<Op,std::uint8_t>,std::uint64_t> responses;
    std::uint64_t replay_attempts=0,retried_unit_attempts=0,host_verification_failures=0;
};
// Exact finite-workload collector. Stores O(work + attempts) records; this is
// measurement storage in the simulator, not modeled device buffering.
// Missing milestones stay optional (JSON null); zero is a valid measured time.
class StatisticsCollector final : public IStatisticsSink {
public:
    void observe(const StatisticsEvent&) override;
    const auto& globals() const {return globals_;}
    const auto& logical_work() const {return work_;}
    const auto& attempts() const {return attempts_;}
    StatisticsSummary summarize(Tick at) const;
    // Schema 1, ticks (not wall-clock time). The caller supplies run stop metadata.
    void write_json(std::ostream&,Tick at,const std::string& stop_reason) const;
private:
    void record(const StatGlobalDefined&); void record(const StatWorkDefined&);
    void record(const StatGlobalReleased&); void record(const StatWorkFinished&);
    void record(const StatGlobalFinished&); void record(const StatAttemptStarted&);
    void record(const StatAttemptLinked&); void record(const StatAttemptAdvanced&);
    void record(const StatAttemptDelivered&); void record(const StatAttemptAssessed&);
    void note(Tick);
    std::map<GlobalRequestId,GlobalTiming> globals_;
    std::map<LogicalWorkId,LogicalTiming> work_;
    std::map<TransferAttemptId,AttemptTiming> attempts_;
    std::map<std::tuple<ChannelId,std::uint64_t,std::uint64_t>,LogicalWorkId> write_index_;
    Tick latest_{};
};
} // namespace hbf_eventlab
