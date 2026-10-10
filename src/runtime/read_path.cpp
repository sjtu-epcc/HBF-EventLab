#include "hbf_eventlab/runtime/read_path.hpp"
#include <utility>

namespace hbf_eventlab {
namespace {
ReadSubmission deferred_read(std::vector<WaitKey> keys) {
    return {SubmitResult::Backpressured,ReadDisposition::Deferred,{},{},std::move(keys)};
}
WaitKey result_space() { return WaitKey::channel_key(WaitReason::ResultSpace,0); }
}
ReadPath::ReadPath(const StaticDeviceDescription& d,const IAddressLayout& layout,
        CapacityRetirementCoordinator& capacity,NandMediaState& media,CommandAdmission& admission,
        const WritePath& writes,IReadResultStorage& results)
    : description_(d),layout_(layout),capacity_(capacity),media_(media),admission_(admission),writes_(writes),results_(results) {
    if (&d!=&writes.description_ || &layout!=&writes.layout_ || &capacity!=&writes.capacity_ ||
        &media!=&writes.media_ || &admission!=&writes.admission_)
        throw std::invalid_argument("read/write paths must share runtime and command ledger");
}
ReadPath::~ReadPath() {
    for (auto& item:parents_) {
        auto& p=item.second;
        if (p.wait) admission_.notifications().cancel(*p.wait);
        if (p.guard) capacity_.release_read(*p.guard,false);
        admission_.read_order().abandon(item.first);
        admission_.abandon_on_shutdown(item.first.first,item.first.second);
    }
}
ReadPath::Parent& ReadPath::accept_parent(const HostCommand& cmd) {
    const CandidateId id{checked_add(next_candidate_,1)};
    if (admission_.try_accept(cmd)!=SubmitResult::Accepted) throw std::logic_error("admission changed after preflight");
    admission_.read_order().accept({cmd.address.channel,cmd.id},cmd.read_group);
    auto& p=parents_.emplace(CommandKey{cmd.address.channel,cmd.id},
        Parent{cmd,id,{},{},{},{},false}).first->second;
    next_candidate_=id.value; return p;
}
ReadPath::Observation ReadPath::observe(const HostCommand& cmd) const {
    const auto logical=layout_.logical_view(cmd.address);
    if (capacity_.retired(logical.block)) return {true,{ReadStatus::InvalidAddress},{}};
    const auto view=writes_.read_view(logical.block);
    if (view.current_contents) {
        const auto forward=writes_.forward({cmd.address.channel,LocalDlu{cmd.address.local.value/dlu_bytes}},
            static_cast<std::uint32_t>(cmd.address.local.value%dlu_bytes/host_unit_bytes));
        if (forward.kind!=ForwardKind::NoPendingWrite) {
            if (cmd.read_group) throw std::invalid_argument("LIMIT-READ-001: ordered Pending reads are unsupported");
            if (forward.kind==ForwardKind::Available) return {true,{ReadStatus::Success},forward.data};
            return {true,{ReadStatus::PendingWriteMissing},{}};
        }
    }
    if (view.replay==ReplayPhase::AwaitingRestart)
        return {true,{ReadStatus::TemporarilyRestricted,ReadRestrictionReason::AwaitingReplay},{}};
    if (view.replay==ReplayPhase::Replaying)
        return {true,{ReadStatus::TemporarilyRestricted,ReadRestrictionReason::ReplayInProgress},{}};
    if (!view.current_contents) return {true,{ReadStatus::UnwrittenPage},{}};
    if (media_.unusable(block_of(capacity_.inspect(cmd.address))))
        return {true,{ReadStatus::TemporarilyRestricted,ReadRestrictionReason::PhysicalRepairRequired},{}};
    return {};
}
ReadSubmission ReadPath::immediate(const HostCommand& cmd,Observation observation) {
    std::optional<ReadResultLease> data;
    if (observation.data) {
        const auto slot=results_.reserve(cmd.address.channel); if (!slot) return deferred_read({result_space()});
        data.emplace(results_,*slot); data->store(*observation.data);
    }
    auto& p=accept_parent(cmd);
    p.response=observation.result; p.result=std::move(data);
    admission_.notifications().notify(WaitKey::channel_key(WaitReason::ResponseReady,cmd.address.channel));
    return {SubmitResult::Accepted,observation.data?ReadDisposition::Forwarded:ReadDisposition::Error,
        observation.result.status,{},{}};
}
ReadSubmission ReadPath::submit(const HostCommand& cmd) {
    if (cmd.op!=Op::Read) throw std::invalid_argument("ReadPath accepts reads only");
    if (admission_.contains(cmd.address.channel,cmd.id)) throw std::logic_error("duplicate outstanding command ID");
    if (!admission_.has_credit(cmd.address.channel))
        return deferred_read({WaitKey::channel_key(WaitReason::Credit,cmd.address.channel)});
    const auto normalized=inspect_command(cmd,description_);
    if (normalized.error) return immediate(cmd,{true,{*normalized.error==CommandError::InvalidAddress?
        ReadStatus::InvalidAddress:ReadStatus::InvalidUserField},{}});
    if (cmd.size_bytes!=host_unit_bytes) throw std::invalid_argument("read burst unsupported by minimal ReadPath");
    const auto observation=observe(cmd); // includes LIMIT-READ-001 before ANY acceptance side effect
    if (observation.terminal) return immediate(cmd,observation);
    const auto logical=layout_.logical_view(cmd.address);
    if (writes_.read_view(logical.block).preparation==BlockPreparation::ErasePending)
        return deferred_read({WaitKey::block_key(WaitReason::BlockProgress,logical.block)});
    const auto guard=capacity_.acquire_read(cmd.address);
    if (!guard) return deferred_read({capacity_.zone_wait(cmd.address)});
    auto& p=accept_parent(cmd); p.guard=guard; ++waiting_;
    const ReadCandidate c{cmd.address.channel,cmd.id,p.identity};
    arm(c);
    return {SubmitResult::Accepted,ReadDisposition::Queued,{},c,{}};
}
bool ReadPath::valid(ReadCandidate c) const {
    const auto it=parents_.find({c.channel,c.command});
    return it!=parents_.end() && it->second.identity==c.identity && !it->second.issued && !it->second.response;
}
std::optional<ReadCandidate> ReadPath::candidate(ChannelId ch,CommandId id) const {
    const auto it=parents_.find({ch,id});
    if (it==parents_.end() || it->second.issued || it->second.response) return {};
    return ReadCandidate{ch,id,it->second.identity};
}
ReadProbe ReadPath::probe(ReadCandidate c, bool require_result_space) const {
    if (!valid(c)) return {ProbeState::Stale,{},{}};
    const auto& p=parents_.at({c.channel,c.command});
    if (observe(p.command).terminal) return {ProbeState::Terminal,{},{}};
    if (!admission_.read_order().head({c.channel,c.command}))
        return {ProbeState::Blocked,{WaitKey::order(c.channel,*p.command.read_group)},{}};
    const auto logical=layout_.logical_view(p.command.address);
    if (writes_.read_view(logical.block).preparation==BlockPreparation::ErasePending)
        return {ProbeState::Blocked,{WaitKey::block_key(WaitReason::BlockProgress,logical.block)},{}};
    if (require_result_space && results_.capacity_bytes()-results_.used_bytes()<host_unit_bytes)
        return {ProbeState::Blocked,{result_space()},{}};
    return {ProbeState::Ready,{},capacity_.inspect(p.command.address)};
}
void ReadPath::complete(Parent& p,Result result,std::optional<ReadResultLease> data) {
    if ((result.status==ReadStatus::Success)!=data.has_value()) throw std::logic_error("read result owner missing");
    if (p.wait) { admission_.notifications().cancel(*p.wait); p.wait.reset(); }
    p.response=result; p.result=std::move(data);
    if (!p.issued) --waiting_;
    if (p.guard) { capacity_.release_read(*p.guard); p.guard.reset(); }
    admission_.notifications().notify(WaitKey::channel_key(WaitReason::ResponseReady,p.command.address.channel));
}
bool ReadPath::resolve_terminal(ReadCandidate c) {
    if (!valid(c)) return false;
    auto& p=parents_.at({c.channel,c.command});
    const auto o=observe(p.command); if (!o.terminal) return false;
    std::optional<ReadResultLease> data;
    if (o.data) {
        const auto slot=results_.reserve(c.channel); if (!slot) return false;
        data.emplace(results_,*slot); data->store(*o.data);
    }
    complete(p,o.result,std::move(data));
    return true;
}
void ReadPath::arm(ReadCandidate c) {
    auto& p=parents_.at({c.channel,c.command});
    if (p.wait) admission_.notifications().cancel(*p.wait);
    const auto logical=layout_.logical_view(p.command.address);
    std::vector<WaitObservation> observations;
    auto& signals=admission_.notifications();
    for (const auto key:{WaitKey::block_key(WaitReason::BlockProgress,logical.block),
                         capacity_.zone_wait(p.command.address),
                         WaitKey::physical(block_of(capacity_.inspect(p.command.address))),result_space()})
        observations.push_back(signals.observe(key));
    p.wait=signals.subscribe(observations,[this,c] {
        if (!valid(c)) return;
        parents_.at({c.channel,c.command}).wait.reset();
        if (!resolve_terminal(c)) arm(c);
    });
}
std::optional<ReadOperation> ReadPath::issue(ReadCandidate c) {
    if (!valid(c) || resolve_terminal(c)) return {};
    const auto pview=probe(c); if (pview.state!=ProbeState::Ready) return {};
    const auto slot=results_.reserve(c.channel); if (!slot) return {};
    return issue_impl(c,ReadResultLease(results_,*slot));
}
std::optional<ReadOperation> ReadPath::issue_reserved(ReadCandidate c, ReadResultLease data) {
    if (!valid(c) || resolve_terminal(c) || probe(c,false).state!=ProbeState::Ready) return {};
    return issue_impl(c,std::move(data));
}
std::optional<ReadOperation> ReadPath::issue_impl(ReadCandidate c, ReadResultLease reserved) {
    auto& p=parents_.at({c.channel,c.command});
    const auto id=ReadOperationId{checked_add(next_operation_,1)};
    std::optional<ReadResultLease> data(std::move(reserved));
    auto address=capacity_.pin_read(p.command.address,*p.guard);
    if (!address) return {}; // Reservation rolls back; no operation or Programming side effect.
    const auto* page=media_.read(address->physical()); // Functional sampling at ISSUE, never submit.
    Result result{page?ReadStatus::Success:ReadStatus::UnwrittenPage};
    if (page) data->store(page->at(address->physical().byte_offset/host_unit_bytes));
    else data.reset();
    ReadOperation operation{id,c.channel,c.command,address->physical(),p.command.axi_id,p.command.read_hint};
    reads_.emplace(id,Active{operation,result,std::move(data),std::move(address)});
    if (p.wait) { admission_.notifications().cancel(*p.wait); p.wait.reset(); }
    p.issued=true; --waiting_; next_operation_=id.value;
    return operation;
}
bool ReadPath::serve_cached(ReadCandidate c, ReadResultLease data) {
    if (!valid(c) || resolve_terminal(c) || probe(c,false).state!=ProbeState::Ready) return false;
    complete(parents_.at({c.channel,c.command}),{ReadStatus::Success},std::move(data));
    return true;
}
std::optional<ReadResultId> ReadPath::result_in(ChannelId ch,CommandId id,const IReadResultStorage& storage) const {
    const auto& p=parents_.at({ch,id});
    if (p.result && p.result->belongs_to(storage)) return p.result->id();
    return {};
}
void ReadPath::transfer_result(ChannelId ch,CommandId id,ReadResultLease destination) {
    auto& p=parents_.at({ch,id});
    if (!p.response || !p.result) throw std::logic_error("no result to transfer");
    destination.store(p.result->load()); p.result=std::move(destination);
}
std::optional<ReadResponse> ReadPath::response(ChannelId ch,CommandId id) const {
    const auto it=parents_.find({ch,id});
    if(it==parents_.end() || !it->second.response || !admission_.read_order().head(it->first)) return {};
    const auto& p=it->second;
    return ReadResponse{ch,id,p.response->status,p.result?std::optional<DataToken>{p.result->load()}:std::nullopt,p.response->restriction};
}
std::vector<ReadOperation> ReadPath::pending_reads() const {
    std::vector<ReadOperation> out; for (const auto& r:reads_) out.push_back(r.second.operation); return out;
}
void ReadPath::finish_read(ReadOperationId id) {
    const auto it=reads_.find(id); if (it==reads_.end()) throw std::logic_error("unknown/stale read operation");
    auto& p=parents_.at({it->second.operation.channel,it->second.operation.command});
    complete(p,it->second.result,std::move(it->second.data));
    reads_.erase(it); // Stable result owner exists before physical lease is released.
    capacity_.publish_releases();
    admission_.notifications().notify(result_space());
}
std::vector<ReadResponse> ReadPath::responses() const {
    std::vector<ReadResponse> out;
    for (const auto& item:parents_) {
        const auto& p=item.second;
        if (p.response && admission_.read_order().head(item.first))
            out.push_back({item.first.first,item.first.second,p.response->status,
                p.result?std::optional<DataToken>{p.result->load()}:std::nullopt,p.response->restriction});
    }
    return out;
}
void ReadPath::response_delivered(ChannelId ch,CommandId id) {
    auto it=parents_.find({ch,id});
    if (it==parents_.end() || !it->second.response || !admission_.read_order().head({ch,id}))
        throw std::logic_error("read response not deliverable/already delivered");
    const auto group=admission_.read_order().delivered({ch,id});
    admission_.response_delivered(ch,id);
    parents_.erase(it);
    auto& signals=admission_.notifications();
    signals.notify(result_space());
    if (group) signals.notify(WaitKey::order(ch,*group));
}
} // namespace hbf_eventlab

