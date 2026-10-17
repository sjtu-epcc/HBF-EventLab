#pragma once
#include "hbf_eventlab/interfaces/host_port.hpp"
#include "hbf_eventlab/interfaces/statistics_sink.hpp"
#include "hbf_eventlab/runtime/read_path.hpp"
#include "hbf_eventlab/runtime/bank_cache.hpp"
#include "hbf_eventlab/runtime/link_buffers.hpp"
#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include "hbf_eventlab/policies/read_dispatch.hpp"
#include "hbf_eventlab/policies/execution_policies.hpp"
#include <variant>
#include <memory>

namespace hbf_eventlab {
struct DeviceActivity {
    std::size_t unsettled_transfers=0, ingress=0, in_flight_requests=0, outstanding=0,
                pending_dlus=0, bank_operations=0, waiting_erases=0, window_reads=0,
                queued_reads=0, egress=0, in_flight_responses=0, feedback_events=0,
                protected_cache_slots=0, filling_cache_slots=0, scheduled_driver_events=0, queued_wakeups=0;
    bool drained() const;
};
struct DeviceStatistics {
    std::uint64_t handoffs=0, request_transfers=0, accepted=0, delivered=0,
                  senses=0, cache_hits=0, programs=0, erases=0, window_batches=0,
                  cache_blocked=0, ingress_attempts=0;
};
struct DeviceTrace {
    const char* action;
    Tick at;
    ChannelId channel;
    std::uint64_t identity;
    std::optional<PhysicalNandAddress> target;
};
// Autonomous local-device execution. HostDriver supplies Global splitting/recovery.
// All mutation lives on the EventQueue thread. Events and supplied policies outlive driver.
class DeviceDriver : public IHostPort {
public:
    using Receiver=std::function<void(const DeliveredResponse&)>;
    DeviceDriver(StaticDeviceDescription,ExecutionConfiguration,EventQueue&,
                 std::unique_ptr<INandSchedulingPolicy> = {},
                 std::unique_ptr<IUcieArbitrationPolicy> = {},
                 std::unique_ptr<IReadDispatchPolicy> = {});
    ~DeviceDriver();
    DeviceDriver(const DeviceDriver&)=delete;
    DeviceDriver& operator=(const DeviceDriver&)=delete;
    // Taken starts serialization and reserves RX; no synchronous callback.
    // NotTaken retains source ownership: subscribe returned keys before retrying.
    HandoffResult try_submit(const LocalTransaction&,Receiver,AcceptanceObserver = {}) override;
    AdmissionWaitRegistry& notifications() { return signals_; }
    const StaticDeviceDescription& description() const { return description_; }
    const ExecutionConfiguration& configuration() const { return config_; }
    const CoreChannelTopology& topology() const { return topology_; }
    const ReferenceAddressLayout& layout() const { return layout_; }
    const NandMediaState& media() const { return media_; }
    const WritePath& writes() const { return writes_; }
    const ReadPath& reads() const { return reads_; }
    const BankCache& cache() const { return cache_; }
    const BufferBudget& ingress(ChannelId ch) const;
    const BufferBudget& egress(ChannelId ch) const { return egress_.budget(ch); }
    const DeviceRuntimeState& resources() const { return runtime_; }
    // Read-only diagnostic for verifying configured retirement budget.
    std::uint64_t capacity_hole_limit(ChannelId ch) const { return capacity_.hole_limit(ch); }
    DeviceActivity activity() const;
    const DeviceStatistics& statistics() const { return stats_; }
    // Attach before the first transfer. No sink means no measurement history.
    void set_statistics_sink(IStatisticsSink&);
    // Fault input and optional streaming diagnostics; no unbounded history in Device.
    void set_program_outcome(std::function<ProgramOutcome(const ProgramOperation&)> f) { outcome_=std::move(f); }
    void set_trace(std::function<void(const DeviceTrace&)> f) { trace_=std::move(f); }
    void declare_discardable(ChannelId,std::uint64_t);
    bool quiesce_pair(ChannelId,std::uint64_t,std::uint64_t);
    void cancel_quiesce(ChannelId,std::uint64_t,std::uint64_t);
    RemapResult commit_swap(ChannelId,std::uint64_t,std::uint64_t);
private:
    enum class Stage { Link, Ingress, Accepted, Egress, ResponseLink, Feedback };
    struct Transaction {
        TransferAttemptId id;
        LocalTransaction input;
        Receiver receiver;
        AcceptanceObserver acceptance;
        Stage stage=Stage::Link;
        std::uint64_t accepted_sequence=0;
        std::optional<WaitId> wait;
        std::optional<ReadCandidate> read;
        bool released=false,cache_pending=false;
        std::optional<ReadResultLease> cache_result;
        std::optional<PhysicalNandAddress> target;
        std::optional<ReadResultId> egress;
        bool egress_in_path=false;
        std::optional<DeviceResponse> delivered;
    };
    struct Channel {
        Channel(BufferCapacity c,const IReadDispatchPolicy& policy):rx(c),window(policy) {}
        BufferBudget rx;
        ReadDispatchWindow window;
        std::optional<EventId> pump,timer;
        std::optional<WaitId> response_wait;
        bool request_busy=false,response_busy=false;
        std::size_t request_flights=0,response_flights=0,feedback=0;
        std::set<std::uint64_t> transactions; // active directory for this channel, never payload copies
        std::map<DluKey,std::uint64_t> programs;
        std::map<CandidateId,std::uint64_t> erases;
    };
    EventId schedule(Tick,EventPhase,std::function<void()>);
    void pump(ChannelId);
    void request_pump(ChannelId);
    void watch_responses(ChannelId);
    void wait(Transaction&,const std::vector<WaitKey>&);
    bool admit(Transaction&);
    void release_batches(ChannelId,const std::vector<DispatchBatch>&);
    void update_timer(ChannelId);
    void remove_window(Transaction&);
    bool try_cache(Transaction&);
    void execute_media(ChannelId);
    void queue_responses(ChannelId);
    void start_response(ChannelId);
    void deliver(std::uint64_t);
    void notify_cache(BankKey);
    void notify_bank(BankKey);
    bool prior_sense(const Transaction&,BankKey) const;
    void notify_sense_order(const Transaction&);
    void emit(const char*,ChannelId,std::uint64_t,std::optional<PhysicalNandAddress> = {});
    Transaction* find(ChannelId,CommandId);
    static WaitKey bank_wait(WaitReason,BankKey);
    StaticDeviceDescription description_;
    ExecutionConfiguration config_;
    EventQueue& events_;
    CoreChannelTopology topology_;
    ReferenceAddressLayout layout_;
    ZoneMapping zones_;
    BlockReplacement replacements_;
    NandMediaState media_;
    CapacityRetirementCoordinator capacity_;
    AdmissionWaitRegistry signals_;
    CommandAdmission admission_;
    BankCache cache_;
    EgressBuffer egress_;
    WritePath writes_;
    ReadPath reads_;
    DeviceRuntimeState runtime_;
    std::unique_ptr<INandSchedulingPolicy> nand_policy_;
    std::unique_ptr<IUcieArbitrationPolicy> link_policy_;
    std::unique_ptr<IReadDispatchPolicy> dispatch_policy_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::map<std::uint64_t,Transaction> transactions_;
    std::map<CommandKey,std::uint64_t> identities_;
    std::set<EventId> events_owned_;
    std::uint64_t next_attempt_=0;
    DeviceStatistics stats_;
    IStatisticsSink* statistics_sink_=nullptr;
    std::function<ProgramOutcome(const ProgramOperation&)> outcome_=[](const auto&){return ProgramOutcome::Success;};
    std::function<void(const DeviceTrace&)> trace_;
};
} // namespace hbf_eventlab


