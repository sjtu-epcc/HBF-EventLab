#pragma once
#include "hbf_eventlab/common/host_protocol.hpp"
#include "hbf_eventlab/runtime/capacity_retirement_coordinator.hpp"
#include "hbf_eventlab/runtime/block_write_order.hpp"
#include "hbf_eventlab/runtime/write_assembly.hpp"
#include "hbf_eventlab/runtime/nand_media_state.hpp"
#include "hbf_eventlab/policies/retained_assembly_storage.hpp"
#include "hbf_eventlab/infrastructure/event_queue.hpp"
#include "hbf_eventlab/runtime/operation_candidate.hpp"

namespace hbf_eventlab {
enum class WriteDisposition { Supplement, NewPage, BeginBlock, RetryPage0, Deferred, Error };
struct WriteSubmission {
    SubmitResult admission;
    WriteDisposition disposition;
    std::optional<WriteStatus> immediate_status;
    std::vector<WaitKey> waits;
};
enum class BlockPreparation { None, ErasePending, Ready };
// A derived view, never a separately maintained read-restriction flag.
struct BlockReadView {
    ReplayPhase replay;
    BlockPreparation preparation;
    bool current_contents;
};
// Outcome is a fault-model input, not inferred from the Host-visible 0x7 alone.
enum class ProgramOutcome { Success, FailedReusable, FailedUnusable };
struct EraseOperation { OperationId id; LogicalBlockKey logical; PhysicalBlockKey physical; };
struct ProgramOperation {
    OperationId id;
    LogicalBlockKey logical;
    DluKey dlu;
    Generation context;
    PhysicalNandAddress physical;
};
// Fixed correctness coordinator, NOT a replaceable ReplayLegalityPolicy.
// Dependencies outlive this object. Assembly deadlines use EventQueue; media completions
// are explicit tickets driven by a harness/future Device. No calibrated timing here.
class WritePath {
public:
    WritePath(const StaticDeviceDescription&, const IAddressLayout&,
              CapacityRetirementCoordinator&, NandMediaState&, EventQueue&, CommandAdmission&);
    ~WritePath();
    WritePath(const WritePath&) = delete;
    WritePath& operator=(const WritePath&) = delete;
    WriteSubmission submit(const HostCommand&, const std::vector<DataToken>&);
    std::vector<EraseCandidate> pending_erases() const;
    std::optional<EraseCandidate> erase_candidate(CandidateId) const;
    PathProbe probe_erase(CandidateId) const;
    std::optional<EraseOperation> issue_erase(CandidateId);
    void finish_erase(OperationId);
    std::optional<ProgramCandidate> program_candidate(DluKey) const;
    PathProbe probe_program(ProgramCandidate) const;
    std::optional<ProgramOperation> issue_program(ProgramCandidate);
    void finish_program(OperationId, ProgramOutcome = ProgramOutcome::Success);
    std::vector<WriteResponse> responses() const;
    std::optional<WriteResponse> response(ChannelId, CommandId) const;
    std::optional<PhysicalNandAddress> program_target(ProgramCandidate) const;
    void set_cache_invalidator(std::function<void()> f) { invalidate_cache_=std::move(f); }
    void response_delivered(ChannelId, CommandId);
    const BlockWriteOrder& order(LogicalBlockKey) const;
    BlockPreparation preparation(LogicalBlockKey) const;
    BlockReadView read_view(LogicalBlockKey) const;
    const CommandAdmission& admission() const { return admission_; }
    const PendingWriteContext* find(DluKey k) const { return assembly_.find(k); }
    ForwardResult forward(DluKey k, std::uint32_t slot) const { return assembly_.forward(k, slot); }
    std::size_t outstanding(ChannelId ch) const { return admission_.outstanding(ch); }
    std::size_t pending(ChannelId ch) const { return assembly_.pending(ch); }
    std::size_t active_programs() const { return programs_.size(); }
    std::size_t active_erases() const { return erases_.size(); }
    std::size_t waiting_erases() const { return erase_candidates_.size(); }
    std::size_t parent_count() const { return parents_.size(); }
private:
    friend class ReadPath; // Constructor verifies one shared runtime, never copies its state.
    struct Block {
        explicit Block(std::uint32_t pages) : order(pages) {}
        BlockWriteOrder order;
        BlockPreparation preparation = BlockPreparation::None;
        std::optional<BlockSlotKey> slot;
        std::optional<PhysicalBlockKey> physical;
        std::size_t outstanding = 0;
        std::optional<Generation> contents;
    };
    struct Binding {
        LogicalBlockKey logical;
        std::shared_ptr<ResolvedAddressLease> address;
        std::optional<EventId> timeout;
    };
    struct Parent {
        std::optional<LogicalBlockKey> logical;
        std::shared_ptr<ResolvedAddressLease> address;
        std::optional<WriteResponse> response;
    };
    struct Erase { EraseOperation operation; std::shared_ptr<ResolvedAddressLease> address; };
    struct WaitingErase { EraseCandidate candidate; std::shared_ptr<ResolvedAddressLease> address; };
    using ParentKey = std::pair<ChannelId, CommandId>;
    Block& block(LogicalBlockKey);
    OperationId next_operation();
    void accept_parent(const HostCommand&, std::optional<LogicalBlockKey>, std::shared_ptr<ResolvedAddressLease>);
    WriteSubmission reject(const HostCommand&, std::optional<LogicalBlockKey>, WriteStatus, std::uint8_t info = 0);
    void complete(DluCompletion);
    void expire(DluKey, Generation);
    DluKey key_for(LogicalBlockKey, PageId) const;
    const StaticDeviceDescription& description_;
    const IAddressLayout& layout_;
    CapacityRetirementCoordinator& capacity_;
    NandMediaState& media_;
    EventQueue& events_;
    CommandAdmission& admission_;
    RetainedAssemblyStorage storage_;
    WriteAssembly assembly_;
    std::map<LogicalBlockKey, Block> blocks_;
    std::map<DluKey, Binding> bindings_;
    std::map<ParentKey, Parent> parents_;
    std::map<OperationId, Erase> erases_;
    std::map<CandidateId, WaitingErase> erase_candidates_;
    std::map<OperationId, ProgramOperation> programs_;
    std::function<void()> invalidate_cache_ = [] {}; // Component-only harness has no cache.
    std::uint64_t next_operation_ = 0;
    std::uint64_t next_candidate_ = 0;
};
} // namespace hbf_eventlab
