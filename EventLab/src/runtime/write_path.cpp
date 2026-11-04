#include "hbf_eventlab/runtime/write_path.hpp"

namespace hbf_eventlab {
namespace {
WriteSubmission deferred(std::vector<WaitKey> keys) { return {SubmitResult::Backpressured, WriteDisposition::Deferred, std::nullopt, std::move(keys)}; }
}
WritePath::WritePath(const StaticDeviceDescription& d, const IAddressLayout& layout,
        CapacityRetirementCoordinator& capacity, NandMediaState& media, EventQueue& events, CommandAdmission& admission)
    : description_(d), layout_(layout), capacity_(capacity), media_(media), events_(events),
      admission_(admission),
      storage_(checked_mul(d.geometry().host_channels, d.capabilities().max_pending_write_dlus)), assembly_(d, storage_) {
    if (admission.channels() != d.geometry().host_channels || admission.limit() != d.configuration().mocs_selected)
        throw std::invalid_argument("shared command ledger must match the device profile");
    if (&admission.notifications().events()!=&events) throw std::invalid_argument("different runtime clocks");
    capacity_.attach_notifications(admission.notifications());
}
WritePath::~WritePath() {
    for (const auto& item : bindings_) if (item.second.timeout) events_.cancel(*item.second.timeout);
    for (const auto& item : parents_) admission_.abandon_on_shutdown(item.first.first, item.first.second);
}
void WritePath::set_statistics_sink(IStatisticsSink& sink) {
    // Assembly owns all transitions. Only this clock-aware bridge adds time;
    // observation does not enqueue callbacks or change path readiness.
    assembly_.set_observer([this,&sink](const AssemblyObservation& e){sink.observe(StatDluObserved{events_.now(),e});});
    for(ChannelId ch=0;ch<description_.geometry().host_channels;++ch)
        sink.observe(StatPendingInitialized{ch,description_.capabilities().max_pending_write_dlus,events_.now()});
}
WritePath::Block& WritePath::block(LogicalBlockKey k) {
    return blocks_.try_emplace(k, description_.geometry().pages_per_block).first->second;
}
const BlockWriteOrder& WritePath::order(LogicalBlockKey k) const { return blocks_.at(k).order; }
BlockPreparation WritePath::preparation(LogicalBlockKey k) const { return blocks_.at(k).preparation; }
BlockReadView WritePath::read_view(LogicalBlockKey k) const {
    const auto contents = capacity_.content_generation(k);
    const auto it = blocks_.find(k);
    if (it == blocks_.end() || !it->second.contents)
        return {ReplayPhase::Idle, BlockPreparation::None, contents == Generation{}};
    if (*it->second.contents != contents)
        return {ReplayPhase::Idle, BlockPreparation::None, false};
    return {it->second.order.replay().phase(), it->second.preparation, true};
}
OperationId WritePath::next_operation() { next_operation_ = checked_add(next_operation_, 1); return OperationId{next_operation_}; }
DluKey WritePath::key_for(LogicalBlockKey k, PageId p) const {
    return {k.channel, LocalDlu{layout_.encode_logical({k, p, 0}).value / dlu_bytes}};
}
void WritePath::accept_parent(const HostCommand& cmd, std::optional<LogicalBlockKey> k, std::shared_ptr<ResolvedAddressLease> address) {
    if (admission_.try_accept(cmd) != SubmitResult::Accepted) throw std::logic_error("admission changed after preflight");
    parents_.emplace(ParentKey{cmd.address.channel, cmd.id}, Parent{k, std::move(address), std::nullopt});
    if (k) ++block(*k).outstanding;
}
WriteSubmission WritePath::reject(const HostCommand& cmd, std::optional<LogicalBlockKey> k, WriteStatus status, std::uint8_t info) {
    // Immediate command errors have no DLU membership and acquire no media lease.
    // DEFERRED(ERR-ASYNC-RETIRE): retirement must separately terminate every affected
    // unfinished DLU member; this helper must never become a fault-fanout shortcut.
    accept_parent(cmd, k, {});
    parents_.at({cmd.address.channel, cmd.id}).response = WriteResponse{cmd.address.channel, cmd.id, status, info};
    admission_.notifications().notify(WaitKey::channel_key(WaitReason::ResponseReady,cmd.address.channel));
    return {SubmitResult::Accepted, WriteDisposition::Error, status};
}
WriteSubmission WritePath::submit(const HostCommand& cmd, const std::vector<DataToken>& data) {
    if (cmd.op != Op::Write) throw std::invalid_argument("WritePath accepts writes only");
    if (admission_.contains(cmd.address.channel, cmd.id)) throw std::logic_error("duplicate outstanding command ID");
    if (!admission_.has_credit(cmd.address.channel)) return deferred({WaitKey::channel_key(WaitReason::Credit,cmd.address.channel)});
    auto normalized = inspect_command(cmd, description_);
    if (normalized.error) return reject(cmd, std::nullopt,
        *normalized.error == CommandError::InvalidAddress ? WriteStatus::InvalidAddress : WriteStatus::InvalidUserField);
    const auto& fragments = normalized.fragments;
    if (data.size() != fragments.size()) throw std::invalid_argument("one token per 64B fragment required");
    const auto logical = layout_.logical_view(cmd.address);
    auto& b = block(logical.block);
    const auto key = fragments.front().key;
    if (capacity_.retired(logical.block)) {
        b.order.retire();
        return reject(cmd, logical.block, WriteStatus::InvalidAddress);
    }
    const auto contents = capacity_.content_generation(logical.block);
    if (b.contents && *b.contents != contents) {
        // A committed Host discard/remap abandons old contents, including replay
        // debt. The generation also catches A->B->A with identical final coordinates.
        if (b.outstanding || b.order.has_active_pages() || b.preparation == BlockPreparation::ErasePending)
            return deferred({WaitKey::block_key(WaitReason::BlockProgress,logical.block)});
        b.order.discard_contents(contents);
        b.preparation = BlockPreparation::None;
        b.slot.reset(); b.physical.reset(); b.contents = contents;
    }
    const auto* current = assembly_.find(key);
    const auto preflight = assembly_.check_command(fragments, events_.now());
    if (current && preflight != WriteStatus::Success) return reject(cmd, logical.block, preflight);

    auto disposition = current ? WriteDisposition::Supplement : WriteDisposition::NewPage;
    std::shared_ptr<ResolvedAddressLease> address;
    if (current) {
        address = bindings_.at(key).address; // Existing DLU supplements can drain a quiescing zone.
    } else {
        // Earlier accepted reads keep their logical contents stable even before issue.
        if ((logical.page==0 && capacity_.logical_reads().needs_block(logical.block)) ||
            capacity_.logical_reads().needs_page(logical.block,logical.page))
            return deferred({WaitKey::block_key(WaitReason::LogicalReaders,logical.block)});
        const auto nominal = capacity_.slot(cmd.address);
        if (nominal.page != logical.page) throw std::logic_error("reference WritePath requires page-preserving mapping");
        const auto physical = capacity_.inspect(cmd.address);
        const bool same_target = b.slot && *b.slot == block_of(nominal) && b.physical && *b.physical == block_of(physical);
        if (logical.page == 0) {
            const bool retry = b.order.mode() == BlockMode::Normal && b.order.next_page_to_assemble() == 0 &&
                               b.preparation != BlockPreparation::None && same_target;
            if (retry) {
                disposition = WriteDisposition::RetryPage0;
            } else {
                // CHOSEN boundary: no old write, response, erase or pinned read may be crossed.
                // Nothing is accepted/queued here: caller retries, and must still supply old DLU holes.
                if (b.order.has_active_pages() || b.outstanding || b.preparation == BlockPreparation::ErasePending ||
                    capacity_.in_flight(block_of(physical))) return deferred({
                        WaitKey::block_key(WaitReason::BlockProgress,logical.block),WaitKey::physical(block_of(physical))});
                disposition = WriteDisposition::BeginBlock;
            }
        } else {
            if (b.order.mode() == BlockMode::AwaitingReplay)
                return reject(cmd, logical.block, WriteStatus::WriteOrderViolation, 2);
            if (!same_target || b.preparation == BlockPreparation::None)
                return reject(cmd, logical.block, WriteStatus::WriteOrderViolation, 1);
            if (logical.page < b.order.next_page_to_program())
                return reject(cmd, logical.block, WriteStatus::OverlapAddress);
            // §11.5.2.7 interpretation: finish the failed prefix before issuing follow-up pages.
            if (b.order.replay_through() && logical.page > *b.order.replay_through() &&
                logical.page == b.order.next_page_to_assemble()) return deferred({WaitKey::block_key(WaitReason::BlockProgress,logical.block)});
            if (!b.order.can_start(logical.page))
                return reject(cmd, logical.block, WriteStatus::WriteOrderViolation, 1);
        }
        if (preflight != WriteStatus::Success) return reject(cmd, logical.block, preflight);
        if (disposition == WriteDisposition::BeginBlock) {
            const auto result = capacity_.repair_block(logical.block, invalidate_cache_);
            if (result == CapacityResult::InFlight) return deferred({capacity_.zone_wait(cmd.address),
                WaitKey::physical(block_of(physical)),WaitKey::block_key(WaitReason::LogicalReaders,logical.block)});
            if (result == CapacityResult::Retired || result == CapacityResult::AlreadyRetired) {
                b.order.retire();
                return reject(cmd, logical.block, result == CapacityResult::Retired ?
                    WriteStatus::CapacityUnusable : WriteStatus::InvalidAddress,
                    result == CapacityResult::Retired ? 1 : 0);
            }
        }
        address = capacity_.pin(cmd.address);
        if (!address) return deferred({capacity_.zone_wait(cmd.address)});
    }

    accept_parent(cmd, logical.block, address);
    if (!current) {
        if (disposition == WriteDisposition::BeginBlock) {
            b.order.restart_from_page0();
            b.slot = block_of(address->slot()); b.physical = block_of(address->physical());
            b.contents = address->content_generation();
            b.order.bind_target(*b.physical, *b.contents);
            b.preparation = BlockPreparation::ErasePending;
            const CandidateId id{checked_add(next_candidate_,1)}; next_candidate_=id.value;
            erase_candidates_.emplace(id, WaitingErase{{id, logical.block, *b.physical}, address});
        }
        b.order.start_page(logical.page);
        bindings_.emplace(key, Binding{logical.block, address, std::nullopt});
    }
    AssemblyResult first{}, last{};
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        last = assembly_.accept(fragments[i], data[i], events_.now());
        if (i == 0) first = last;
        if (last.status != WriteStatus::Success) throw std::logic_error("preflight/assembly inconsistency");
    }
    auto& binding = bindings_.at(key);
    if (last.became_ready) {
        b.order.assembly_complete(logical.page);
        if (binding.timeout) { events_.cancel(*binding.timeout); binding.timeout.reset(); }
    } else if (first.opened) {
        const auto generation = first.generation;
        binding.timeout = events_.schedule_at(first.deadline, EventPhase::Deadline,
            [this, key, generation] { expire(key, generation); });
    }
    admission_.notifications().notify(WaitKey::dlu(key));
    admission_.notifications().notify(WaitKey::block_key(WaitReason::BlockProgress,logical.block));
    return {SubmitResult::Accepted, disposition, std::nullopt};
}
std::vector<EraseCandidate> WritePath::pending_erases() const {
    std::vector<EraseCandidate> out;
    for (const auto& item : erase_candidates_) out.push_back(item.second.candidate);
    return out;
}
std::optional<EraseCandidate> WritePath::erase_candidate(CandidateId id) const {
    const auto it=erase_candidates_.find(id);
    return it==erase_candidates_.end()?std::nullopt:std::optional<EraseCandidate>{it->second.candidate};
}
PathProbe WritePath::probe_erase(CandidateId id) const {
    const auto it=erase_candidates_.find(id);
    if (it==erase_candidates_.end()) return {ProbeState::Stale,{}};
    const auto& c=it->second.candidate;
    if (capacity_.logical_reads().needs_block(c.logical))
        return {ProbeState::Blocked,{WaitKey::block_key(WaitReason::LogicalReaders,c.logical)}};
    return {ProbeState::Ready,{}};
}
std::optional<EraseOperation> WritePath::issue_erase(CandidateId candidate) {
    if (probe_erase(candidate).state!=ProbeState::Ready) return {};
    const auto& c=erase_candidates_.at(candidate);
    const auto id=next_operation();
    EraseOperation op{id,c.candidate.logical,c.candidate.physical};
    erases_.emplace(id,Erase{op,c.address}); erase_candidates_.erase(candidate); return op;
}
void WritePath::finish_erase(OperationId id) {
    const auto op = erases_.at(id).operation;
    media_.erase(op.physical);
    block(op.logical).preparation = BlockPreparation::Ready;
    erases_.erase(id);
    capacity_.publish_releases();
    admission_.notifications().notify(WaitKey::block_key(WaitReason::BlockProgress,op.logical));
}
std::optional<ProgramCandidate> WritePath::program_candidate(DluKey k) const {
    const auto* ctx=assembly_.find(k);
    if (!ctx) return {};
    return ProgramCandidate{k,ctx->generation};
}
PathProbe WritePath::probe_program(ProgramCandidate candidate) const {
    const auto* ctx=assembly_.find(candidate.dlu);
    if (!ctx || ctx->generation!=candidate.context) return {ProbeState::Stale,{}};
    const auto& binding=bindings_.at(candidate.dlu);
    const auto& b=blocks_.at(binding.logical);
    if (ctx->phase!=WritePhase::Ready || b.preparation!=BlockPreparation::Ready || !b.order.can_program(binding.address->physical().page))
        return {ProbeState::Blocked,{WaitKey::dlu(candidate.dlu),WaitKey::block_key(WaitReason::BlockProgress,binding.logical)}};
    return {ProbeState::Ready,{}};
}
std::optional<WriteResponse> WritePath::response(ChannelId ch,CommandId id) const {
    const auto it=parents_.find({ch,id});
    return it==parents_.end()?std::nullopt:it->second.response;
}
std::optional<PhysicalNandAddress> WritePath::program_target(ProgramCandidate c) const {
    if (probe_program(c).state==ProbeState::Stale) return {};
    return bindings_.at(c.dlu).address->physical();
}
std::optional<ProgramOperation> WritePath::issue_program(ProgramCandidate candidate) {
    if (probe_program(candidate).state!=ProbeState::Ready) return {};
    const auto k=candidate.dlu;
    const auto* ctx=assembly_.find(k);
    const auto& binding = bindings_.at(k);
    auto& b = block(binding.logical);
    auto physical = binding.address->physical(); physical.byte_offset = 0;
    if (b.preparation != BlockPreparation::Ready || !b.order.can_program(physical.page)) return std::nullopt;
    const auto id = next_operation();
    ProgramOperation op{id, binding.logical, k, ctx->generation, physical};
    programs_.emplace(id, op);
    assembly_.queue_program(k, ctx->generation);
    assembly_.start_program(k, ctx->generation);
    b.order.start_program(physical.page);
    return op;
}
void WritePath::complete(DluCompletion completion) {
    auto& binding = bindings_.at(completion.key);
    if (binding.timeout) events_.cancel(*binding.timeout);
    for (const auto id : completion.commands) {
        auto& parent = parents_.at({completion.key.channel, id});
        if (parent.response) throw std::logic_error("command completed twice");
        parent.response = WriteResponse{completion.key.channel, id, completion.status, completion.additional_info};
    }
    bindings_.erase(completion.key); // Parents keep leases until responses are delivered.
    admission_.notifications().notify(WaitKey::dlu(completion.key));
    admission_.notifications().notify(WaitKey::channel_key(WaitReason::ResponseReady,completion.key.channel));
}
void WritePath::expire(DluKey k, Generation generation) {
    auto completion = assembly_.expire(k, generation, events_.now());
    if (!completion) return;
    const auto logical = bindings_.at(k).logical;
    const auto page = bindings_.at(k).address->slot().page;
    block(logical).order.assembly_timeout(page);
    complete(std::move(*completion)); // Preparation/erase survives a page-0 timeout.
    admission_.notifications().notify(WaitKey::block_key(WaitReason::BlockProgress,logical));
}
void WritePath::finish_program(OperationId id, ProgramOutcome outcome) {
    const auto op = programs_.at(id); // Missing/stale ticket cannot affect a newer operation.
    auto& b = block(op.logical);
    if (outcome == ProgramOutcome::Success) {
        auto done = assembly_.commit_program(op.dlu, op.context,
            [&](const PageData& data) { media_.program(op.physical, data); });
        b.order.program_success(op.physical.page);
        complete(std::move(done));
    } else {
        if (outcome == ProgramOutcome::FailedUnusable) capacity_.mark_unusable(block_of(op.physical));
        const auto failure = b.order.program_failure(op.physical.page);
        complete(assembly_.fail_program(op.dlu, op.context));
        for (const auto page : failure.following_pages) {
            const auto key = key_for(op.logical, page);
            const auto* ctx = assembly_.find(key);
            if (!ctx) throw std::logic_error("order/assembly follower mismatch");
            complete(assembly_.fail_prior_program(key, ctx->generation));
        }
        b.preparation = BlockPreparation::None;
    }
    programs_.erase(id);
    admission_.notifications().notify(WaitKey::block_key(WaitReason::BlockProgress,op.logical));
    capacity_.publish_releases();
}
std::vector<WriteResponse> WritePath::responses() const {
    std::vector<WriteResponse> out;
    for (const auto& item : parents_) if (item.second.response) out.push_back(*item.second.response);
    return out; // Inspection; not an AXI ordering implementation.
}
void WritePath::response_delivered(ChannelId ch, CommandId id) {
    auto it = parents_.find({ch, id});
    if (it == parents_.end() || !it->second.response) throw std::logic_error("response not ready/already delivered");
    admission_.response_delivered(ch, id);
    const auto logical=it->second.logical;
    if (logical) --block(*logical).outstanding;
    parents_.erase(it);
    capacity_.publish_releases();
    if (logical) admission_.notifications().notify(WaitKey::block_key(WaitReason::BlockProgress,*logical));
}
} // namespace hbf_eventlab
