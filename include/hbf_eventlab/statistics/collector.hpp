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
struct PendingCounts {
    std::uint64_t total=0,collecting=0,ready_waiting=0,programming=0;
};
struct PendingIntegrals {
    long double total=0,collecting=0,ready_waiting=0,programming=0;
};
struct PendingSummary {
    Tick observed_from;
    std::uint32_t limit=0;
    PendingCounts current,peak;
    PendingIntegrals area,mean; // Area: context*tick; mean: contexts.
    std::uint64_t at_limit_ticks=0,opened=0,retired=0;
};
struct PendingSample { Tick at; std::uint64_t sequence; ChannelId channel; PendingCounts counts; };
enum class DluTermination { ProgramSuccess, ProgramFail, PriorProgramFail, AssemblyTimeout };
const char* to_string(DluTermination);
using DluContextKey=std::tuple<ChannelId,std::uint64_t,std::uint64_t>; // channel, local DLU, generation
struct DluProgress {
    Tick at; std::uint64_t sequence; AssemblyChange change; WritePhase phase;
    std::uint32_t received_fragments;
    std::optional<CommandId> command;
};
struct DluTiming {
    DluKey key; Generation generation; Tick opened_at,deadline;
    std::optional<Tick> ready_at,program_started_at,retired_at;
    WritePhase phase=WritePhase::Collecting;
    std::uint32_t received_fragments=0;
    std::optional<DluTermination> termination;
    std::optional<WriteStatus> status;
    std::uint8_t additional_info=0;
    // Capture attempt identity at membership time: CommandId may later be reused.
    // A standalone WritePath observer has no transport attempt, represented by null.
    std::map<CommandId,std::optional<TransferAttemptId>> members;
    std::vector<DluProgress> progress;
};
struct BankTiming {
    std::uint64_t ticket; MediaOperationKind kind; PhysicalNandAddress target;
    Tick issued_at; std::optional<Tick> completed_at;
};
struct StatisticsSummary {
    Tick at;
    std::map<Op,ResultCounts> globals,logical_work,attempts;
    // Keys include read/write, level, and outcome. No mixed-status latency pool.
    std::map<std::string,LatencySummary> latency;
    std::map<std::pair<Op,std::uint8_t>,std::uint64_t> responses;
    std::map<ChannelId,PendingSummary> pending_channels;
    std::uint64_t replay_attempts=0,retried_unit_attempts=0,host_verification_failures=0;
};
// Exact finite-workload collector. Stores O(work + attempts + assembly changes); this is
// measurement storage in the simulator, not modeled device buffering.
// Missing milestones stay optional (JSON null); zero is a valid measured time.
class StatisticsCollector final : public IStatisticsSink {
public:
    static constexpr unsigned schema_version=2;
    void observe(const StatisticsEvent&) override;
    const auto& globals() const {return globals_;}
    const auto& logical_work() const {return work_;}
    const auto& attempts() const {return attempts_;}
    const auto& pending_timeline() const {return pending_timeline_;}
    const auto& dlu_contexts() const {return dlus_;}
    const auto& bank_operations() const {return bank_operations_;}
    StatisticsSummary summarize(Tick at) const;
    // Schema 2, ticks (not wall-clock time). The caller supplies run stop metadata.
    void write_json(std::ostream&,Tick at,const std::string& stop_reason) const;
private:
    void record(const StatGlobalDefined&); void record(const StatWorkDefined&);
    void record(const StatGlobalReleased&); void record(const StatWorkFinished&);
    void record(const StatGlobalFinished&); void record(const StatAttemptStarted&);
    void record(const StatAttemptLinked&); void record(const StatAttemptAdvanced&);
    void record(const StatAttemptDelivered&); void record(const StatAttemptAssessed&);
    void record(const StatPendingInitialized&); void record(const StatDluObserved&);
    std::map<ChannelId,PendingSummary> pending_summary(Tick) const;
    void write_pending_json(std::ostream&,Tick) const;
    void record(const StatBankOperationStarted&); void record(const StatBankOperationFinished&);
    void write_bank_json(std::ostream&,Tick) const;
    std::map<std::uint64_t,BankTiming> bank_operations_;
    std::map<std::tuple<ChannelId,std::uint32_t,std::uint32_t>,std::uint64_t> active_banks_;
    void note(Tick);
    std::map<GlobalRequestId,GlobalTiming> globals_;
    std::map<LogicalWorkId,LogicalTiming> work_;
    std::map<TransferAttemptId,AttemptTiming> attempts_;
    std::map<std::pair<ChannelId,CommandId>,TransferAttemptId> command_attempts_;
    struct PendingState { PendingSummary summary; Tick last; };
    std::map<ChannelId,PendingState> pending_;
    std::map<DluContextKey,DluTiming> dlus_;
    std::map<DluKey,Generation> live_dlus_;
    std::vector<PendingSample> pending_timeline_;
    std::uint64_t next_pending_sequence_=0;
    std::map<std::tuple<ChannelId,std::uint64_t,std::uint64_t>,LogicalWorkId> write_index_;
    Tick latest_{};
};
} // namespace hbf_eventlab
