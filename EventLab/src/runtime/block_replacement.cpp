#include "hbf_eventlab/runtime/block_replacement.hpp"
#include <map>
#include <set>
#include <utility>

namespace hbf_eventlab {
struct ReplacementState {
    Geometry geometry;
    std::map<BlockSlotKey, std::uint64_t> entries;
    // Lease accounting is shared by every staged mapping snapshot. Unrelated
    // outstanding leases must still decrement the current snapshot's counters.
    std::shared_ptr<std::map<BlockSlotKey, std::uint64_t>> active =
        std::make_shared<std::map<BlockSlotKey, std::uint64_t>>();
    std::map<std::tuple<ChannelId, std::uint32_t, std::uint32_t>, std::uint64_t> next_spare;
    std::set<PhysicalBlockKey> retired;
    void validate(BlockSlotKey k) const {
        if (k.channel >= geometry.host_channels || k.die >= geometry.dies_per_channel ||
            k.bank >= geometry.banks_per_die || k.block >= geometry.blocks_per_bank)
            throw std::out_of_range("nominal block slot outside logical geometry");
    }
};
ReplacementLease::ReplacementLease(std::shared_ptr<ReplacementState> s, BlockSlotKey k, PhysicalNandAddress a)
    : state_(std::move(s)), slot_(k), physical_(a) {}
ReplacementLease::ReplacementLease(ReplacementLease&& other) noexcept
    : state_(std::move(other.state_)), slot_(other.slot_), physical_(other.physical_) {}
ReplacementLease::~ReplacementLease() { if (state_) --state_->active->at(slot_); }
BlockReplacement::BlockReplacement(Geometry g) : state_(std::make_shared<ReplacementState>()) {
    g.validate(); state_->geometry = g;
}
PhysicalNandAddress BlockReplacement::translate(const NandSlotAddress& a) const {
    const auto k = block_of(a); state_->validate(k);
    if (a.page >= state_->geometry.pages_per_block || a.byte_offset >= dlu_bytes)
        throw std::out_of_range("slot page/offset outside geometry");
    auto it = state_->entries.find(k);
    const auto target = it == state_->entries.end() ? k.block : it->second;
    return {a.channel, a.die, a.bank, target, a.page, a.byte_offset};
}
ReplacementLease BlockReplacement::pin(const NandSlotAddress& a) {
    if (managed_) throw std::logic_error("use CapacityRetirementCoordinator for managed mappings");
    return pin_impl(a);
}
ReplacementLease BlockReplacement::pin_impl(const NandSlotAddress& a) {
    const auto physical = translate(a);
    auto& active = (*state_->active)[block_of(a)];
    active = checked_add(active, 1);
    return ReplacementLease{state_, block_of(a), physical};
}
std::uint64_t BlockReplacement::in_flight(BlockSlotKey k) const {
    state_->validate(k);
    auto it = state_->active->find(k);
    return it == state_->active->end() ? 0 : it->second;
}
bool BlockReplacement::retired(PhysicalBlockKey k) const { return state_->retired.count(k) != 0; }
bool BlockReplacement::has_pins() const {
    for (const auto& e : *state_->active) if (e.second) return true;
    return false;
}
BlockReplacement::Plan BlockReplacement::plan_replace(BlockSlotKey k, PhysicalBlockKey failed,
        const std::function<bool(PhysicalBlockKey)>& healthy) const {
    state_->validate(k);
    if (in_flight(k)) return {ReplacementResult::InFlight, {}};
    auto staged = std::make_shared<ReplacementState>(*state_);
    staged->retired.insert(failed); // The actual target may come from RetiredHoleMap.
    auto& used = staged->next_spare[std::make_tuple(k.channel, k.die, k.bank)];
    while (used < staged->geometry.spare_blocks_per_bank) {
        const PhysicalBlockKey candidate{k.channel, k.die, k.bank, staged->geometry.blocks_per_bank + used};
        ++used;
        if (!healthy(candidate) || staged->retired.count(candidate)) {
            staged->retired.insert(candidate);
            continue;
        }
        staged->entries[k] = candidate.block;
        return {ReplacementResult::Applied, std::move(staged)};
    }
    return {ReplacementResult::NoSpare, std::move(staged)};
}
ReplacementResult BlockReplacement::replace_failed(BlockSlotKey k, const std::function<void()>& invalidate) {
    if (managed_) throw std::logic_error("use CapacityRetirementCoordinator for managed mappings");
    const auto failed = block_of(translate({k.channel, k.die, k.bank, k.block, 0, 0}));
    auto plan = plan_replace(k, failed, [](PhysicalBlockKey) { return true; });
    if (plan.result != ReplacementResult::Applied) return plan.result;
    if (!invalidate) throw std::invalid_argument("replacement requires invalidation boundary");
    invalidate();
    commit(plan);
    return ReplacementResult::Applied;
}
} // namespace hbf_eventlab
