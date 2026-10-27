#pragma once
#include "hbf_eventlab/host/global_request_splitter.hpp"
#include "hbf_eventlab/host/retention_store.hpp"
#include "hbf_eventlab/host/recovery_manager.hpp"
#include "hbf_eventlab/policies/host_send_policy.hpp"
#include "hbf_eventlab/interfaces/statistics_sink.hpp"
#include <bitset>
#include <memory>
namespace hbf_eventlab {
struct HostActivity {
    std::size_t unfinished_global=0,queued_units=0,unsettled_attempts=0,recoveries=0,events=0,subscriptions=0;
    bool drained() const {return !(unfinished_global||queued_units||unsettled_attempts||recoveries||events||subscriptions);}
};
struct HostStatistics {
    std::uint64_t handed=0,accepted=0,responses=0,attempt_errors=0,logical_success=0,logical_failed=0,
        recovery_tasks=0,replay_passes=0,replay_commands=0,recovered_fragments=0,
        // Error counts are command responses; retry rounds are whole-page rounds.
        pending_limit_errors=0,pending_retry_commands=0,assembly_timeout_errors=0,
        timeout_retry_rounds=0,timeout_retry_commands=0,order_retry_commands=0;
};
struct HostTrace {
    const char* action; Tick at; CommandId command; SendUnitId unit;
    LogicalBlockKey block; std::uint64_t pass;
};
struct GlobalCompletion {
    WorkResult result;
    Tick at; // Final logical outcome time, not callback consumption time.
};
enum class InputDisposition { Registered, Backpressured, TooLarge };
struct InputCapacity {
    std::size_t limit=0,resident=0;
    std::uint64_t generation=0; // increases when resident work is reclaimed
};
struct InputAdmission { InputDisposition disposition; std::uint64_t fragments; InputCapacity capacity; };
struct HostStorage {
    std::size_t requests,results,work,units,attempts,versions,retained;
};
class HostDriver {
public:
    HostDriver(IHostPort&,EventQueue&,const IHostInterleaver&,HostConfiguration={},
               std::unique_ptr<IHostSendPolicy> = {});
    ~HostDriver();
    HostDriver(const HostDriver&)=delete;
    HostDriver& operator=(const HostDriver&)=delete;
    void load(const std::vector<GlobalRequest>&); // fully validate before start/any transmission
    void start();
    // Alternative to load/start: one persistent Host, single EventQueue thread.
    // Optional initial batch is validated before the mode/start transition.
    void start_incremental(const std::vector<GlobalRequest>& initial = {});
    // Batch validation is atomic for input-contract errors; no Device activity.
    // Existing or same-batch dependencies only. Burst pages must be complete in
    // this batch. not_before is absolute simulation time (must not be in the past).
    // Convenience submit throws if capacity rejects; try_submit returns normal
    // backpressure without registration, callbacks, IDs, or content sealing.
    void submit(const std::vector<GlobalRequest>&);
    void submit(const GlobalRequest& request) { submit(std::vector<GlobalRequest>{request}); }
    InputAdmission try_submit(const std::vector<GlobalRequest>&);
    InputAdmission try_submit(const GlobalRequest& r) { return try_submit(std::vector<GlobalRequest>{r}); }
    InputAdmission try_start_incremental(const std::vector<GlobalRequest>&);
    InputCapacity input_capacity() const { return {config_.max_resident_fragments,work_.size(),input_generation_}; }
    HostStorage storage() const;
    // A coalesced event on reclaimed capacity, not a guarantee a batch fits.
    void set_input_capacity_handler(std::function<void(InputCapacity)>);
    bool incremental_input() const { return incremental_; }
    // Attach once before load/start. Delivered asynchronously, exactly once for
    // each final Global result. The callback may submit but must not run events.
    void set_completion_handler(std::function<void(const GlobalCompletion&)>);
    // Use the same sink as DeviceDriver, before load. The sink outlives this Host.
    void set_statistics_sink(IStatisticsSink&);
    // Optional program diagnostics. Attach once before input/start; outlives Host.
    void set_runtime_profile(RuntimeProfile&);
    RuntimeProfile* runtime_profile() const { return profile_; }
    HostActivity activity() const;
    const std::map<GlobalRequestId,WorkResult>& results() const {return results_;}
    const HostStatistics& statistics() const {return stats_;}
    const RecoveryManager& recovery() const {return recovery_;}
    const WriteRetentionStore& retained() const {return retention_;}
    void set_trace(std::function<void(const HostTrace&)> f) {trace_=std::move(f);}
    bool workload_success() const;
    std::string waiting_reason() const;
private:
    enum class UnitStage { Queued,Handed,Done,Cancelled };
    enum class RetryKind { None, PendingLimit, Timeout, Order };
    enum class RetryPhase { Repairing, Draining, AwaitingPredecessor };
    struct PageRetry {
        bool active=false,order_explained=false;
        RetryPhase phase=RetryPhase::Repairing;
        std::uint64_t pass=0;
        std::uint32_t timeout_rounds=0;
    };
    struct Work {Fragment fragment;LogicalResult result=LogicalResult::Pending;bool needs_recovery=false;bool accepted=false;};
    struct Unit {
        SendUnitId id;
        Op op;
        std::vector<Fragment> fragments;
        std::optional<ContentKey> content;
        std::optional<ReadGroup> group;
        ReadHint hint=ReadHint::Regular;
        UnitStage stage=UnitStage::Queued;
        bool replay=false,accepted=false,successful=false;
        std::uint64_t pass=0;
        std::optional<CommandId> command;
        RetryKind retry=RetryKind::None;
        bool order_retry_allowed=false;
        std::uint32_t pending_retries=0,assembly_round=0;
        std::uint64_t handoffs=0;
        std::uint64_t capacity_at_handoff=0;
        Tick retry_at{};
        Tick queued_at{}; // logical enqueue boundary, also exported to statistics
        std::uint64_t enqueue_sequence=0; // 0: preloaded but not released yet
    };
    struct Version {
        std::vector<SendUnitId> normal;
        // Host input coverage, not Device received/Assembly state. Survives retries.
        std::map<PageId,std::bitset<fragments_per_dlu>> defined;
        std::map<PageId,std::bitset<fragments_per_dlu>> accepted;
        std::map<PageId,std::bitset<fragments_per_dlu>> successful;
        std::size_t unfinished=0;
        // Acceptance is tentative send progress, not a mirror of Device Assembly.
        std::map<PageId,PageRetry> retries;
        std::uint64_t next_pass=0;
        bool aborted=false;
        std::string reason;
    };
    struct PageKey {
        ContentKey content;PageId page;
        friend bool operator<(const PageKey&a,const PageKey&b){return std::tie(a.content,a.page)<std::tie(b.content,b.page);}
    };
    struct Channel {
        std::set<SendUnitId> queue;
        std::map<PageKey,std::size_t> open;
        std::optional<WaitId> wait;
        std::optional<EventId> pump;
    };
    struct Attempt {SendUnitId unit;TransferAttemptId transfer;bool accepted=false;};
    struct PreparedRequest { GlobalRequest request; std::vector<Fragment> fragments; };
    std::vector<PreparedRequest> prepare(const std::vector<GlobalRequest>&) const;
    InputAdmission admit_input(const std::vector<GlobalRequest>&,bool initial);
    void reclaim(); // only at stable callback boundaries, never while iterating units
    void register_input(std::vector<PreparedRequest>);
    void arm_releases(const std::vector<GlobalRequestId>&);
    SendUnitId add_unit(Op,std::vector<Fragment>,std::optional<ContentKey>,std::optional<ReadGroup>,bool replay=false,std::uint64_t pass=0);
    void enqueue(Unit&,Tick);
    void release_units(Tick);
    bool dependencies_ready(GlobalRequestId) const;
    bool eligible(const Unit&) const;
    bool version_done(ContentKey) const;
    void request_pump(ChannelId);
    void pump(ChannelId);
    void accepted(CommandId,const AcceptedNotice&);
    void receive(CommandId,const DeliveredResponse&);
    void advance();
    bool advance_recovery(ContentKey,RecoveryTask&);
    void make_replay(ContentKey,RecoveryTask&);
    void abort_version(ContentKey,const std::string&);
    bool retry_eligible(const Unit&) const;
    void note_write_retry(Unit&,const WriteResponse&);
    bool advance_write_retries();
    void requeue(Unit&);
    bool page_succeeded(ContentKey,PageId,std::uint64_t pass) const;
    void update_retry_wakeup();
    void finish_work(LogicalWorkId,bool,const std::string& = {});
    void cancel_unit(Unit&,const std::string&);
    void emit(const char*,const Unit&,CommandId={});
    EventId schedule(Tick,std::function<void()>);
    IHostPort& port_;
    EventQueue& events_;
    GlobalRequestSplitter splitter_;
    HostConfiguration config_;
    std::unique_ptr<IHostSendPolicy> policy_;
    WriteRetentionStore retention_;
    RecoveryManager recovery_;
    std::vector<Channel> channels_;
    std::map<GlobalRequestId,GlobalRequest> requests_;
    std::map<GlobalRequestId,WorkResult> results_;
    std::map<LogicalWorkId,Work> work_;
    std::map<SendUnitId,Unit> units_;
    std::map<ContentKey,Version> versions_;
    std::map<LogicalBlockKey,std::vector<ContentKey>> version_order_;
    std::map<CommandId,Attempt> attempts_;
    std::map<LogicalBlockKey,std::set<CommandId>> unsettled_;
    std::set<EventId> owned_events_;
    std::map<Tick,EventId> release_events_;
    std::map<Tick,std::vector<GlobalRequestId>> release_globals_;
    // Preindexed initial releases: (last contributing input-fragment ordinal, unit).
    // Burst creation order and arbitrary Global IDs must not define FCFS order.
    std::map<Tick,std::vector<std::pair<std::uint64_t,SendUnitId>>> initial_enqueues_;
    std::optional<EventId> retry_wakeup_;
    std::optional<Tick> retry_wakeup_at_;
    std::map<ChannelId,std::uint64_t> capacity_progress_;
    std::set<std::tuple<ContentKey,PageId,std::uint64_t,std::uint32_t>> capacity_completions_;
    std::shared_ptr<bool> alive_=std::make_shared<bool>(true);
    std::uint64_t next_work_=0,next_unit_=0,next_command_=0;
    std::uint64_t next_enqueue_=0;
    bool loaded_=false,started_=false,incremental_=false;
    HostStatistics stats_;
    RuntimeProfile* profile_=nullptr;
    IStatisticsSink* statistics_sink_=nullptr;
    std::function<void(const HostTrace&)> trace_;
    std::function<void(const GlobalCompletion&)> completion_;
    std::function<void(InputCapacity)> input_capacity_handler_;
    std::optional<EventId> input_capacity_event_;
    std::uint64_t input_generation_=0;
};
} // namespace hbf_eventlab

