#include "hbf_eventlab/runtime/capacity_retirement_coordinator.hpp"
#include <algorithm>
#include <utility>

namespace hbf_eventlab {
struct PhysicalLeaseState {
    struct Entry {
        std::uint64_t count=0;
        bool released=false;
        PhysicalBlockKey key{};
        std::set<std::pair<ChannelId,std::uint64_t>> zones;
        Entry* next=nullptr;
    };
    std::map<PhysicalBlockKey,Entry> active;
    Entry* released_head=nullptr;
};
namespace {
struct TransactionGuard {
    bool& busy;
    explicit TransactionGuard(bool& value) : busy(value) { busy = true; }
    ~TransactionGuard() { busy = false; }
};
bool same_lane(PhysicalBlockKey p, BlockSlotKey s) {
    return p.channel == s.channel && p.die == s.die && p.bank == s.bank;
}
}
PhysicalMappingLease::PhysicalMappingLease(std::shared_ptr<PhysicalLeaseState> s, PhysicalBlockKey p)
    : state_(std::move(s)), physical_(p) {}
PhysicalMappingLease::PhysicalMappingLease(PhysicalMappingLease&& other) noexcept
    : state_(std::move(other.state_)), physical_(other.physical_) {}
PhysicalMappingLease::~PhysicalMappingLease() {
    if (state_) {
        auto& entry=state_->active.at(physical_);
        if (--entry.count==0 && !entry.released) {
            entry.released=true;
            entry.next=state_->released_head;
            state_->released_head=&entry; // Stable map node; no allocation/callback.
        }
    }
}
CapacityRetirementCoordinator::CapacityRetirementCoordinator(const StaticDeviceDescription& d,
        const IAddressLayout& layout, ZoneMapping& zones, BlockReplacement& replacement,
        NandMediaState& media, CapacityPolicy policy)
    : description_(d), layout_(layout), zones_(zones), replacement_(replacement), media_(media),
      policy_(policy), leases_(std::make_shared<PhysicalLeaseState>()) {
    policy.validate();
    if (zones.managed_ || replacement.managed_ || zones.has_pins() || replacement.has_pins())
        throw std::logic_error("capacity coordinator requires unattached, drained dependencies");
    zones.managed_ = replacement.managed_ = true;
}
CapacityRetirementCoordinator::~CapacityRetirementCoordinator() {
    zones_.managed_ = replacement_.managed_ = false;
}
void CapacityRetirementCoordinator::require_idle() const {
    if (busy_) throw std::logic_error("capacity transaction callback cannot reenter mutations or IO");
}
ChannelAddress CapacityRetirementCoordinator::address(LogicalBlockKey k) const {
    return {k.channel, layout_.encode_logical({k, 0, 0})};
}
std::uint64_t CapacityRetirementCoordinator::zone_of(LogicalBlockKey k) const {
    return address(k).local.value / dlu_bytes / description_.configuration().zone_size_dlus;
}
std::vector<LogicalBlockKey> CapacityRetirementCoordinator::blocks(ChannelId ch) const {
    const auto& g = description_.geometry();
    if (ch >= g.host_channels) throw std::out_of_range("channel outside geometry");
    std::vector<LogicalBlockKey> out;
    for (std::uint64_t block = 0; block < g.blocks_per_bank; ++block)
        for (std::uint32_t die = 0; die < g.dies_per_channel; ++die)
            for (std::uint32_t bank = 0; bank < g.banks_per_die; ++bank) out.push_back({ch, die, bank, block});
    return out;
}
bool CapacityRetirementCoordinator::retired(LogicalBlockKey k) const {
    address(k); // Validate even when the sparse set has no entry.
    return retired_.count(k) != 0;
}
std::uint64_t CapacityRetirementCoordinator::hole_limit(ChannelId ch) const {
    const auto& g = description_.geometry();
    if (ch >= g.host_channels) throw std::out_of_range("channel outside geometry");
    const auto n = checked_mul(g.blocks_per_bank, checked_mul(g.dies_per_channel, g.banks_per_die));
    // Avoid overflow in n * permille for large nominal geometries.
    return (n / 1000) * policy_.max_hole_permille + (n % 1000) * policy_.max_hole_permille / 1000;
}
Generation CapacityRetirementCoordinator::content_generation(LogicalBlockKey k) const {
    auto it = contents_.find({k.channel, zone_of(k)});
    return it == contents_.end() ? Generation{} : it->second;
}
NandSlotAddress CapacityRetirementCoordinator::slot(ChannelAddress a) const { return layout_.decode(zones_.translate(a)); }
PhysicalNandAddress CapacityRetirementCoordinator::resolve_unchecked(ChannelAddress a) const {
    const auto s = slot(a);
    const auto hole = holes_.lookup(block_of(s));
    if (!hole) return replacement_.translate(s);
    return {hole->channel, hole->die, hole->bank, hole->block, s.page, s.byte_offset};
}
PhysicalNandAddress CapacityRetirementCoordinator::inspect(ChannelAddress a) const {
    if (retired(layout_.logical_view(a).block)) throw std::out_of_range("retired logical block");
    return resolve_unchecked(a); // Diagnostic inspection may show a target awaiting repair.
}
std::shared_ptr<ResolvedAddressLease> CapacityRetirementCoordinator::pin(ChannelAddress a) {
    return pin_impl(a,false);
}
std::shared_ptr<ResolvedAddressLease> CapacityRetirementCoordinator::pin_read(ChannelAddress a, LogicalReadId id) {
    if (!logical_reads_.matches(id,layout_.logical_view(a),content_generation(layout_.logical_view(a).block)))
        throw std::logic_error("stale/wrong logical read guard");
    return pin_impl(a,true);
}
std::shared_ptr<ResolvedAddressLease> CapacityRetirementCoordinator::pin_impl(ChannelAddress a, bool existing_reader) {
    require_idle();
    const auto physical = inspect(a);
    if (media_.unusable(block_of(physical))) throw std::logic_error("unusable physical block awaits repair");
    auto zone = zones_.pin_impl(a,existing_reader);
    if (!zone) return {};
    const auto s = layout_.decode(zone->media());
    auto replacement = replacement_.pin_impl(s);
    auto& active = leases_->active[block_of(physical)];
    active.key=block_of(physical);
    active.zones.insert({a.channel,zone_of(layout_.logical_view(a).block)});
    active.count = checked_add(active.count, 1);
    PhysicalMappingLease physical_lease{leases_, block_of(physical)};
    return std::make_shared<ResolvedAddressLease>(std::move(*zone), std::move(replacement),
        std::move(physical_lease), s, physical, content_generation(layout_.logical_view(a).block));
}
std::uint64_t CapacityRetirementCoordinator::in_flight(PhysicalBlockKey p) const {
    auto it = leases_->active.find(p);
    return it == leases_->active.end() ? 0 : it->second.count;
}
void CapacityRetirementCoordinator::mark_unusable(PhysicalBlockKey p) {
    require_idle(); media_.mark_unusable(p);
    if (notifications_) notifications_->notify(WaitKey::physical(p));
}
CapacityResult CapacityRetirementCoordinator::repair_block(LogicalBlockKey k, const std::function<void()>& invalidate) {
    require_idle();
    if (retired(k)) return CapacityResult::AlreadyRetired;
    const auto a = address(k);
    const auto s = block_of(slot(a));
    const auto failed = block_of(resolve_unchecked(a));
    if (!media_.unusable(failed)) return CapacityResult::Ready;
    if (zones_.quiesced(a) || logical_reads_.needs_block(k) || in_flight(failed) || replacement_.in_flight(s)) return CapacityResult::InFlight;
    if (!invalidate) throw std::invalid_argument("repair requires synchronous cache invalidation");
    TransactionGuard guard(busy_);
    auto plan = replacement_.plan_replace(s, failed, [&](PhysicalBlockKey p) { return !media_.unusable(p); });
    if (plan.result == ReplacementResult::InFlight) return CapacityResult::InFlight;
    auto holes = holes_.entries(); holes.erase(s);
    auto retired = retired_;
    const bool exhausted = plan.result == ReplacementResult::NoSpare;
    if (exhausted) retired.insert(k);
    const Generation next{checked_add(generation_.value, 1)};
    invalidate(); // Everything that can allocate has been staged; callback sees the old view.
    replacement_.commit(plan);
    holes_.commit(holes); retired_.swap(retired); generation_ = next;
    if (notifications_) notifications_->notify(WaitKey::block_key(WaitReason::BlockProgress,k));
    return exhausted ? CapacityResult::Retired : CapacityResult::Replaced;
}
void CapacityRetirementCoordinator::declare_discardable(ChannelId ch, std::uint64_t z) {
    require_idle(); zones_.declare_discardable_impl(ch, z);
}
bool CapacityRetirementCoordinator::quiesce_pair(ChannelId ch, std::uint64_t a, std::uint64_t b) {
    require_idle(); return zones_.quiesce_pair_impl(ch, a, b);
}
void CapacityRetirementCoordinator::cancel_quiesce(ChannelId ch, std::uint64_t a, std::uint64_t b) {
    require_idle(); zones_.cancel_quiesce_impl(ch, a, b);
    notify_zone(ch,a); notify_zone(ch,b);
}
RemapResult CapacityRetirementCoordinator::commit_swap(ChannelId ch, std::uint64_t a, std::uint64_t b,
        const std::function<void()>& invalidate) {
    require_idle();
    const auto status = zones_.swap_status(ch, a, b);
    if (status != RemapResult::Applied) return status;
    if (!invalidate) throw std::invalid_argument("swap requires synchronous cache invalidation");
    TransactionGuard guard(busy_);
    const auto zd = description_.configuration().zone_size_dlus;
    const auto target_a = zones_.mapped_zone(ch, a), target_b = zones_.mapped_zone(ch, b);
    struct Owner { LogicalBlockKey logical; BlockSlotKey slot; PhysicalBlockKey old, preferred; };
    std::vector<Owner> owners;
    std::set<PhysicalBlockKey> free;
    auto holes = holes_.entries();
    // Gather ACTUAL ownership of the two logical zones. Physical zone membership
    // is not ownership once sparse compensation exists. Outside owners never enter this pool.
    for (auto k : blocks(ch)) {
        const auto z = zone_of(k);
        if (z != a && z != b) continue;
        if (logical_reads_.needs_block(k)) return RemapResult::InFlight;
        holes.erase(block_of(slot(address(k))));
        if (retired(k)) continue;
        const auto old = block_of(resolve_unchecked(address(k)));
        // DEFERRED(REMAP-FAILED-TARGET): this planner requires a healthy ownership
        // pool. Coordinating failed-target metadata repair/retirement with remap
        // is separate work; this result must NOT require Host replay of contents
        // already declared invalid, nor create a quiesce/replay dependency.
        if (media_.unusable(old)) return RemapResult::RecoveryRequired;
        if (in_flight(old)) return RemapResult::InFlight;
        if (!free.insert(old).second) throw std::logic_error("aliased ownership before swap");
        const auto local = address(k).local.value / dlu_bytes;
        const auto s = layout_.decode({ch, MediaDlu{(z == a ? target_b : target_a) * zd + local % zd}, 0});
        if (s.page != 0 || s.channel != k.channel || s.die != k.die || s.bank != k.bank)
            throw std::logic_error("capacity coordinator requires lane/page-preserving zone mappings");
        owners.push_back({k, block_of(s), old, block_of(replacement_.translate(s))});
    }
    std::map<BlockSlotKey, PhysicalBlockKey> assigned;
    // First maximize default mappings; then keep prior targets when possible.
    for (const auto& o : owners) if (free.erase(o.preferred)) assigned.emplace(o.slot, o.preferred);
    for (const auto& o : owners) {
        if (assigned.count(o.slot)) continue;
        auto it = free.find(o.old);
        if (it == free.end() || !same_lane(*it, o.slot))
            it = std::find_if(free.begin(), free.end(), [&](PhysicalBlockKey p) { return same_lane(p, o.slot); });
        if (it == free.end()) return RemapResult::NoCompatibleTarget;
        assigned.emplace(o.slot, *it); free.erase(it);
    }
    if (!free.empty()) throw std::logic_error("swap lost physical ownership");
    for (const auto& o : owners) if (assigned.at(o.slot) != o.preferred) holes[o.slot] = assigned.at(o.slot);
    std::size_t count = 0;
    for (const auto& h : holes) if (h.first.channel == ch) ++count;
    if (count > hole_limit(ch)) return RemapResult::DensityLimit;
    auto contents = contents_;
    for (auto z : {a, b}) {
        auto& generation = contents[{ch, z}];
        generation = Generation{checked_add(generation.value, 1)};
    }
    const Generation next{checked_add(generation_.value, 1)};
    // ZoneMapping prechecks its generation overflow before invoking this callback.
    // The no-throw map swaps below and its permutation update form one commit.
    const auto result = zones_.commit_swap_impl(ch, a, b, [&] {
        invalidate();
        holes_.commit(holes); contents_.swap(contents); generation_ = next;
    });
    notify_zone(ch,a); notify_zone(ch,b);
    return result;
}
void CapacityRetirementCoordinator::attach_notifications(AdmissionWaitRegistry& notifications) {
    if (notifications_ && notifications_ != &notifications) throw std::invalid_argument("runtime notification registries differ");
    notifications_=&notifications;
}
WaitKey CapacityRetirementCoordinator::zone_wait(ChannelAddress a) const {
    return WaitKey::zone(a.channel,a.local.value/dlu_bytes/description_.configuration().zone_size_dlus);
}
void CapacityRetirementCoordinator::notify_zone(ChannelId ch,std::uint64_t z) {
    if (notifications_) notifications_->notify(WaitKey::zone(ch,z));
}
std::optional<LogicalReadId> CapacityRetirementCoordinator::acquire_read(ChannelAddress a) {
    require_idle();
    if (retired(layout_.logical_view(a).block)) throw std::logic_error("logical guard on retired block");
    if (!zones_.begin_logical_use(a)) return {};
    return logical_reads_.acquire(layout_.logical_view(a),content_generation(layout_.logical_view(a).block));
}
void CapacityRetirementCoordinator::release_read(LogicalReadId id,bool notify) {
    const auto b=logical_reads_.release(id);
    if (notify && notifications_) {
        notifications_->notify(WaitKey::block_key(WaitReason::LogicalReaders,b));
        notifications_->notify(zone_wait(address(b)));
    }
}
void CapacityRetirementCoordinator::publish_releases() {
    require_idle();
    while (leases_->released_head) {
        auto* entry=leases_->released_head;
        leases_->released_head=entry->next;
        entry->released=false; entry->next=nullptr;
        if (notifications_) notifications_->notify(WaitKey::physical(entry->key));
        for (const auto& zone:entry->zones) notify_zone(zone.first,zone.second);
        if (!entry->count) entry->zones.clear();
    }
}
void CapacityRetirementCoordinator::validate_invariants(bool require_healthy) const {
    std::set<PhysicalBlockKey> owners;
    std::set<BlockSlotKey> live_slots;
    const auto& g = description_.geometry();
    for (ChannelId ch = 0; ch < g.host_channels; ++ch) {
        if (holes_.size(ch) > hole_limit(ch)) throw std::logic_error("compensation density exceeded");
        for (auto k : blocks(ch)) {
            const auto a = address(k);
            const auto s = block_of(slot(a));
            if (retired(k)) {
                if (holes_.lookup(s)) throw std::logic_error("retired address has compensation entry");
                continue;
            }
            live_slots.insert(s);
            const auto p = block_of(resolve_unchecked(a));
            if (!same_lane(p, s) || p.block >= checked_add(g.blocks_per_bank, g.spare_blocks_per_bank))
                throw std::logic_error("physical target outside compatible geometry");
            if (!owners.insert(p).second) throw std::logic_error("two usable logical blocks alias one physical block");
            if (require_healthy && media_.unusable(p)) throw std::logic_error("usable address requires physical recovery");
        }
    }
    for (const auto& h : holes_.entries()) {
        if (!live_slots.count(h.first)) throw std::logic_error("orphan compensation entry");
        const auto& s = h.first;
        if (block_of(replacement_.translate({s.channel, s.die, s.bank, s.block, 0, 0})) == h.second)
            throw std::logic_error("redundant compensation entry");
    }
}
NandSlotAddress AddressResolver::slot(ChannelAddress a) const { return capacity_.slot(a); }
PhysicalNandAddress AddressResolver::inspect(ChannelAddress a) const { return capacity_.inspect(a); }
std::shared_ptr<ResolvedAddressLease> AddressResolver::pin(ChannelAddress a) { return capacity_.pin(a); }
} // namespace hbf_eventlab
