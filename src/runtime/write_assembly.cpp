#include "hbf_eventlab/runtime/write_assembly.hpp"
#include <algorithm>

namespace hbf_eventlab {
WriteAssembly::WriteAssembly(const StaticDeviceDescription& d, IAssemblyStorage& storage)
    : channel_dlus_(d.geometry().channel_dlus()), limit_(d.capabilities().max_pending_write_dlus),
      timeout_(d.configuration().assembly_timeout), storage_(storage), counts_(d.geometry().host_channels) {
    if (storage.capacity() < checked_mul(counts_.size(), limit_))
        throw std::invalid_argument("retained-data baseline needs one full slot per advertised pending DLU");
}
WriteAssembly::~WriteAssembly() {
    for (const auto& item : contexts_) storage_.release(item.second.data_slot);
}
const PendingWriteContext* WriteAssembly::find(DluKey k) const {
    auto it = contexts_.find(k); return it == contexts_.end() ? nullptr : &it->second;
}
std::size_t WriteAssembly::pending(ChannelId ch) const { return counts_.at(ch); }
WriteStatus WriteAssembly::check_command(const std::vector<CommandFragment>& fs, Tick now) const {
    if (fs.empty() || fs.size() > fragments_per_dlu) throw std::invalid_argument("empty/oversized command");
    const auto& first = fs.front();
    if (first.key.channel >= counts_.size() || first.key.dlu.value >= channel_dlus_)
        throw std::out_of_range("DLU outside channel");
    std::bitset<fragments_per_dlu> mask;
    for (std::size_t i = 0; i < fs.size(); ++i) {
        const auto& f = fs[i];
        if (f.key.channel != first.key.channel || f.key.dlu != first.key.dlu || f.parent != first.parent ||
            f.slot >= fragments_per_dlu || f.index_in_command != i || f.fragments_in_command != fs.size() || mask.test(f.slot))
            throw std::invalid_argument("invalid normalized fragment group");
        mask.set(f.slot);
    }
    const auto* ctx = find(first.key);
    if (!ctx) return pending(first.key.channel) == limit_ ? WriteStatus::PendingDluLimit : WriteStatus::Success;
    if (now < ctx->first_arrival || (ctx->phase == WritePhase::Collecting && ctx->deadline < now))
        throw std::logic_error("events must retire expired context before arrival");
    return ctx->phase != WritePhase::Collecting || (ctx->received & mask).any()
        ? WriteStatus::OverlapAddress : WriteStatus::Success;
}
AssemblyResult WriteAssembly::accept(const CommandFragment& f, DataToken data, Tick now) {
    if (f.key.channel >= counts_.size() || f.key.dlu.value >= channel_dlus_ || f.slot >= fragments_per_dlu ||
        !f.fragments_in_command || f.fragments_in_command > fragments_per_dlu || f.index_in_command >= f.fragments_in_command)
        throw std::invalid_argument("invalid write fragment");
    auto it = contexts_.find(f.key);
    bool opened = false;
    if (it == contexts_.end()) {
        if (counts_[f.key.channel] == limit_)
            return {WriteStatus::PendingDluLimit, Generation{}, false, false, Tick{}};
        PendingWriteContext ctx;
        ctx.key = f.key;
        ctx.generation = Generation{checked_add(next_generation_, 1)};
        ctx.first_arrival = now;
        ctx.deadline = Tick{checked_add(now.value, timeout_.value)};
        ctx.awaiting_completion.push_back(f.parent);
        ctx.data_slot = storage_.allocate();
        try { it = contexts_.emplace(f.key, std::move(ctx)).first; }
        catch (...) { storage_.release(ctx.data_slot); throw; }
        next_generation_ = it->second.generation.value;
        ++counts_[f.key.channel];
        opened = true;
    }
    auto& ctx = it->second;
    if (now < ctx.first_arrival) throw std::logic_error("write arrival moved backwards in time");
    if (ctx.phase == WritePhase::Collecting && ctx.deadline < now)
        throw std::logic_error("expired context must be retired by its timeout event before arrival");
    if (ctx.received.test(f.slot) || ctx.phase != WritePhase::Collecting)
        return {WriteStatus::OverlapAddress, ctx.generation, false, false, ctx.deadline};
    if (std::find(ctx.awaiting_completion.begin(), ctx.awaiting_completion.end(), f.parent) == ctx.awaiting_completion.end())
        ctx.awaiting_completion.push_back(f.parent);
    storage_.write(ctx.data_slot, f.slot, data);
    ctx.received.set(f.slot);
    const bool ready = ctx.received.all();
    if (ready) ctx.phase = WritePhase::Ready;
    return {WriteStatus::Success, ctx.generation, opened, ready, ctx.deadline};
}
ForwardResult WriteAssembly::forward(DluKey k, std::uint32_t slot) const {
    if (slot >= fragments_per_dlu) throw std::out_of_range("fragment slot");
    const auto* ctx = find(k);
    if (!ctx) return {ForwardKind::NoPendingWrite, DataToken{}};
    if (!ctx->received.test(slot)) return {ForwardKind::MissingFragment, DataToken{}};
    return {ForwardKind::Available, storage_.page(ctx->data_slot)[slot]};
}
PendingWriteContext& WriteAssembly::require(DluKey k, Generation gen) {
    auto& ctx = contexts_.at(k);
    if (ctx.generation != gen) throw std::logic_error("stale DLU generation");
    return ctx;
}
void WriteAssembly::queue_program(DluKey k, Generation gen) {
    auto& ctx = require(k, gen);
    if (ctx.phase != WritePhase::Ready) throw std::logic_error("DLU not Ready");
    ctx.phase = WritePhase::ProgramQueued;
}
void WriteAssembly::start_program(DluKey k, Generation gen) {
    auto& ctx = require(k, gen);
    if (ctx.phase != WritePhase::ProgramQueued) throw std::logic_error("DLU not ProgramQueued");
    ctx.phase = WritePhase::Programming;
}
DluCompletion WriteAssembly::retire(DluKey k, WriteStatus status, std::uint8_t info) {
    auto it = contexts_.find(k);
    auto& ctx = it->second;
    DluCompletion out{k, ctx.generation, status, info, std::move(ctx.awaiting_completion)};
    storage_.release(ctx.data_slot);
    contexts_.erase(it);
    --counts_[k.channel];
    return out;
}
DluCompletion WriteAssembly::commit_program(DluKey k, Generation gen, const std::function<void(const PageData&)>& sink) {
    auto& ctx = require(k, gen);
    if (ctx.phase != WritePhase::Programming || !sink) throw std::logic_error("invalid program commit");
    sink(storage_.page(ctx.data_slot));
    return retire(k, WriteStatus::Success, 0);
}
DluCompletion WriteAssembly::fail_program(DluKey k, Generation gen) {
    if (require(k, gen).phase != WritePhase::Programming) throw std::logic_error("program failure outside Programming");
    return retire(k, WriteStatus::ProgramFail, 0);
}
DluCompletion WriteAssembly::fail_prior_program(DluKey k, Generation gen) {
    if (require(k, gen).phase == WritePhase::Programming) throw std::logic_error("later page already programming");
    return retire(k, WriteStatus::WriteOrderViolation, 2);
}
std::optional<DluCompletion> WriteAssembly::expire(DluKey k, Generation gen, Tick now) {
    const auto* ctx = find(k);
    if (!ctx || ctx->generation != gen || ctx->phase != WritePhase::Collecting || now < ctx->deadline)
        return std::nullopt;
    return retire(k, WriteStatus::AssemblyTimeout, 0);
}
} // namespace hbf_eventlab
