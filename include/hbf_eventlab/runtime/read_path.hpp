#pragma once
#include "hbf_eventlab/common/host_protocol.hpp"
#include "hbf_eventlab/runtime/write_path.hpp"
#include "hbf_eventlab/interfaces/read_result_storage.hpp"

namespace hbf_eventlab {
enum class ReadDisposition { Deferred, Error, Forwarded, Queued };
struct ReadSubmission {
    SubmitResult admission;
    ReadDisposition disposition;
    std::optional<ReadStatus> immediate_status;
    std::optional<ReadCandidate> candidate;
    std::vector<WaitKey> waits;
};
struct ReadOperation {
    ReadOperationId id;
    ChannelId channel;
    CommandId command;
    PhysicalNandAddress physical;
    std::uint16_t axi_id;
    ReadHint hint;
};
struct ReadProbe {
    ProbeState state;
    std::vector<WaitKey> waits;
    std::optional<PhysicalNandAddress> target; // Inspection hint only; issue resolves again.
};
// Acceptance never reads NAND or obtains a physical lease. The caller releases
// its window, checks resources, then issues a candidate. No submit==issue mode.
// All dependencies (including result storage) must outlive this path.
class ReadPath {
public:
    ReadPath(const StaticDeviceDescription&,const IAddressLayout&,
             CapacityRetirementCoordinator&,NandMediaState&,CommandAdmission&,const WritePath&,IReadResultStorage&);
    ~ReadPath();
    ReadPath(const ReadPath&) = delete;
    ReadPath& operator=(const ReadPath&) = delete;
    ReadSubmission submit(const HostCommand&);
    ReadProbe probe(ReadCandidate, bool require_result_space = true) const;
    std::optional<ReadCandidate> candidate(ChannelId,CommandId) const;
    // Terminal conditions bypass resource/window waits, but preserve response ordering.
    bool resolve_terminal(ReadCandidate);
    std::optional<ReadOperation> issue(ReadCandidate);
    // Driver pre-reserves a protected Cache destination; no Egress space needed for sense.
    std::optional<ReadOperation> issue_reserved(ReadCandidate, ReadResultLease);
    bool serve_cached(ReadCandidate, ReadResultLease);
    std::optional<ReadResultId> result_in(ChannelId, CommandId, const IReadResultStorage&) const;
    void transfer_result(ChannelId, CommandId, ReadResultLease);
    std::optional<ReadResponse> response(ChannelId, CommandId) const;
    std::vector<ReadOperation> pending_reads() const; // issued operations, not waiting commands
    void finish_read(ReadOperationId);
    std::vector<ReadResponse> responses() const; // only deliverable group heads
    void response_delivered(ChannelId,CommandId);
    std::size_t outstanding(ChannelId ch) const { return admission_.outstanding(ch); }
    std::size_t waiting() const { return waiting_; }
    std::size_t active_reads() const { return reads_.size(); }
    std::size_t parent_count() const { return parents_.size(); }
private:
    struct Result { ReadStatus status; ReadRestrictionReason restriction=ReadRestrictionReason::None; };
    struct Observation { bool terminal=false; Result result{ReadStatus::Success}; std::optional<DataToken> data; };
    struct Parent {
        HostCommand command;
        CandidateId identity;
        std::optional<LogicalReadId> guard;
        std::optional<Result> response;
        std::optional<ReadResultLease> result;
        std::optional<WaitId> wait;
        bool issued=false;
    };
    struct Active {
        ReadOperation operation;
        Result result;
        std::optional<ReadResultLease> data;
        std::shared_ptr<ResolvedAddressLease> address;
    };
    std::optional<ReadOperation> issue_impl(ReadCandidate, ReadResultLease);
    Observation observe(const HostCommand&) const;
    Parent& accept_parent(const HostCommand&);
    bool valid(ReadCandidate) const;
    ReadSubmission immediate(const HostCommand&,Observation);
    void complete(Parent&,Result,std::optional<ReadResultLease>);
    void arm(ReadCandidate);
    const StaticDeviceDescription& description_;
    const IAddressLayout& layout_;
    CapacityRetirementCoordinator& capacity_;
    NandMediaState& media_;
    CommandAdmission& admission_;
    const WritePath& writes_;
    IReadResultStorage& results_;
    std::map<CommandKey,Parent> parents_;
    std::map<ReadOperationId,Active> reads_;
    std::size_t waiting_=0;
    std::uint64_t next_operation_=0,next_candidate_=0;
};
} // namespace hbf_eventlab

