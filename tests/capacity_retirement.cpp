#include "hbf_eventlab/runtime/capacity_retirement_coordinator.hpp"
#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include <iostream>
#include <random>
#include <sstream>
#include <string>

using namespace hbf_eventlab;
namespace {
std::size_t checks = 0;
void check(bool b, const char* why) { ++checks; if (!b) throw std::runtime_error(why); }
template<class F> void rejects(F f) {
    bool threw = false; try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, "expected diagnostic exception");
}
StaticDeviceDescription profile(Geometry g, std::uint64_t groups) {
    return {g, {256, 16, true, true}, {256, Tick{100}, groups * g.block_group_dlus()}};
}
struct Fixture {
    StaticDeviceDescription d;
    ReferenceAddressLayout layout;
    ZoneMapping zones;
    BlockReplacement replacement;
    NandMediaState media;
    CapacityRetirementCoordinator capacity;
    AddressResolver resolver;
    std::uint64_t groups;
    explicit Fixture(Geometry g = {2, 1, 1, 8, 20, 0}, std::uint32_t permille = 50, std::uint64_t group_count = 4)
        : d(profile(g, group_count)), layout(g), zones(d), replacement(g), media(g),
          capacity(d, layout, zones, replacement, media, CapacityPolicy{permille}), resolver(capacity), groups(group_count) {}
    ChannelAddress address(LogicalBlockKey k, PageId p = 0, std::uint32_t offset = 0) const {
        return {k.channel, layout.encode_logical({k, p, offset})};
    }
    PhysicalBlockKey target(LogicalBlockKey k) const { return block_of(resolver.inspect(address(k))); }
    void retire(LogicalBlockKey k) {
        capacity.mark_unusable(target(k));
        check(capacity.repair_block(k, [] {}) == CapacityResult::Retired, "no spare retires exactly one logical block");
    }
    void prepare(std::uint64_t a, std::uint64_t b, ChannelId ch = 0) {
        capacity.declare_discardable(ch, a); capacity.declare_discardable(ch, b);
        check(capacity.quiesce_pair(ch, a, b), "pair quiesced");
    }
    RemapResult swap(std::uint64_t a, std::uint64_t b, ChannelId ch = 0) {
        prepare(a, b, ch); return capacity.commit_swap(ch, a, b, [] {});
    }
    std::vector<LogicalBlockKey> keys() const {
        const auto& g = d.geometry(); std::vector<LogicalBlockKey> out;
        for (ChannelId ch = 0; ch < g.host_channels; ++ch)
            for (std::uint64_t b = 0; b < g.blocks_per_bank; ++b)
                for (std::uint32_t die = 0; die < g.dies_per_channel; ++die)
                    for (std::uint32_t bank = 0; bank < g.banks_per_die; ++bank) out.push_back({ch, die, bank, b});
        return out;
    }
    std::string snapshot() const {
        std::ostringstream out; out << capacity.generation().value << ';';
        for (auto k : keys()) {
            out << capacity.retired(k) << ':' << capacity.content_generation(k).value << ':';
            out << resolver.slot(address(k)).block << ':';
            if (!capacity.retired(k)) out << target(k).block;
            out << ';';
        }
        for (const auto& e : capacity.holes().entries())
            out << e.first.channel << ',' << e.first.die << ',' << e.first.bank << ',' << e.first.block << '>' << e.second.block << ';';
        return out.str();
    }
};
void capacity_compensation() {
    Fixture f;
    const LogicalBlockKey a{0,0,0,0}, b{0,0,0,4};
    f.retire(a);
    const auto count = f.capacity.retired_blocks().size();
    const auto donor = f.target(b);
    f.media.erase(donor);
    check(f.capacity.hole_limit(0) == 1, "default density is floor(20 * 5%)");
    check(f.swap(0, 1) == RemapResult::Applied, "one hole fits low density budget");
    check(f.capacity.retired(a), "retired logical address never revives after swap");
    rejects([&] { f.resolver.inspect(f.address(a)); });
    rejects([&] { f.resolver.pin(f.address(a)); });
    check(f.target(b) == donor, "B retains original healthy physical block through compensation");
    check(f.resolver.slot(f.address(b)).block == 0, "B nominally received A physical zone");
    const auto final = f.resolver.inspect(f.address(b, 3, 192));
    check(final.block == donor.block && final.page == 3 && final.byte_offset == 192, "hole resolution preserves page and byte offset");
    check(f.capacity.holes().size(0) == 1 && f.capacity.holes().size(1) == 0, "per-channel sparse accounting");
    check(f.media.erase_count(donor) == 1, "remap does not erase or transfer wear");
    check(f.capacity.retired_blocks().size() == count, "no extra logical capacity loss");
    f.capacity.validate_invariants(true);
    check(f.swap(0, 1) == RemapResult::Applied, "reverse swap allowed");
    check(f.capacity.holes().size(0) == 0, "redundant hole removed by reverse swap");
    check(f.capacity.content_generation(b) == Generation{2}, "ABA has distinct content generation");
    check(f.target(b) == donor, "reverse swap restored default ownership");
    f.capacity.validate_invariants(true);
}
void capacity_density() {
    Fixture f;
    f.retire({0,0,0,0}); f.retire({0,0,0,1});
    f.prepare(0,1); const auto before = f.snapshot(); bool invalidated = false;
    check(f.capacity.commit_swap(0,0,2,[] {}) == RemapResult::NotQuiesced, "wrong pair cannot commit");
    check(f.capacity.commit_swap(0,0,1,[&] { invalidated = true; }) == RemapResult::DensityLimit, "two compensations exceed budget one");
    check(!invalidated && before == f.snapshot(), "density refusal has no transactional side effects");
    check(!f.resolver.pin(f.address({0,0,0,2})), "refused swap remains quiesced until explicit cancel");
    f.capacity.cancel_quiesce(0,0,1);
    f.retire({0,0,0,4}); f.retire({0,0,0,5});
    check(f.swap(0,1) == RemapResult::Applied, "matching retired offsets need no compensation");
    check(f.capacity.holes().size(0) == 0 && f.capacity.retired_blocks().size() == 4, "matching retirements preserve capacity");
    f.capacity.validate_invariants(true);
    Fixture tiny({1,1,1,8,4,0},50,1);
    tiny.retire({0,0,0,0});
    check(tiny.capacity.hole_limit(0) == 0, "small profiles receive no implicit one-entry exception");
    check(tiny.swap(0,1) == RemapResult::DensityLimit, "zero budget rejects required compensation");
    tiny.capacity.cancel_quiesce(0,0,1);
    check(tiny.swap(1,2) == RemapResult::Applied, "zero budget still allows healthy swap");
}
void capacity_atomicity() {
    Fixture f;
    const LogicalBlockKey k{0,0,0,0};
    f.capacity.mark_unusable(f.target(k));
    const auto failed = f.snapshot();
    rejects([&] { f.capacity.repair_block(k, [] { throw std::runtime_error("invalidate"); }); });
    check(f.snapshot() == failed && !f.capacity.retired(k), "throw leaves no partial logical retirement");
    check(f.capacity.repair_block(k, [] {}) == CapacityResult::Retired, "retry retirement commits");
    f.prepare(0,1); const auto before = f.snapshot();
    rejects([&] { f.capacity.commit_swap(0,0,1,[&] {
        check(f.snapshot() == before, "callback sees coherent old view");
        throw std::runtime_error("invalidate");
    }); });
    check(f.snapshot() == before, "throw rolls back hole map, zone map and generations");
    rejects([&] { f.capacity.commit_swap(0,0,1,[&] { f.capacity.cancel_quiesce(0,0,1); }); });
    check(f.snapshot() == before, "reentrant mutation fails before commit");
    check(f.capacity.commit_swap(0,0,1,[&] {
        rejects([&] { f.resolver.pin(f.address({0,0,0,8})); });
        rejects([&] { f.capacity.mark_unusable({0,0,0,8}); });
        rejects([&] { f.zones.cancel_quiesce(0,0,1); });
    }) == RemapResult::Applied, "guarded callbacks can commit after rejecting reentry");
    rejects([&] { f.zones.pin(f.address({0,0,0,4})); });
    rejects([&] { f.replacement.pin(f.resolver.slot(f.address({0,0,0,4}))); });
    rejects([&] { f.replacement.replace_failed({0,0,0,0}, [] {}); });
    rejects([&] { CapacityRetirementCoordinator duplicate(f.d,f.layout,f.zones,f.replacement,f.media); });
    f.capacity.validate_invariants(true);
    Fixture spare({2,1,1,8,20,2});
    const LogicalBlockKey other{0,0,0,1};
    auto held = spare.resolver.pin(spare.address(other));
    spare.capacity.mark_unusable(spare.target(k));
    const auto old = spare.snapshot();
    rejects([&] { spare.capacity.repair_block(k, [] { throw std::runtime_error("invalidate"); }); });
    check(spare.snapshot() == old, "replacement callback failure leaves all maps unchanged");
    check(spare.capacity.repair_block(k, [] {}) == CapacityResult::Replaced, "replacement retry succeeds");
    check(spare.target(k).block == 20, "failed transaction did not consume first spare");
    check(spare.replacement.in_flight({0,0,0,1}) == 1, "unrelated lease survives snapshot commit");
    held.reset();
    check(spare.replacement.in_flight({0,0,0,1}) == 0 && spare.capacity.in_flight({0,0,0,1}) == 0,
          "old lease decrements shared counters after mapping snapshot swap");
    spare.capacity.validate_invariants(true);
    Fixture skip({2,1,1,8,20,2});
    skip.capacity.mark_unusable({0,0,0,20}); skip.capacity.mark_unusable(skip.target(k));
    check(skip.capacity.repair_block(k, [] {}) == CapacityResult::Replaced && skip.target(k).block == 21,
          "unusable unallocated spare is skipped");
}
void capacity_hole_failure() {
    Fixture f;
    const LogicalBlockKey a{0,0,0,0}, b{0,0,0,4};
    f.retire(a); check(f.swap(0,1) == RemapResult::Applied, "create compensated owner");
    const auto physical = f.target(b);
    auto read = f.resolver.pin(f.address(b));
    f.capacity.mark_unusable(physical);
    check(f.capacity.repair_block(b, [] {}) == CapacityResult::InFlight, "actual hole target cannot change under a read");
    read.reset();
    const auto before = f.snapshot();
    rejects([&] { f.capacity.repair_block(b, [] { throw std::runtime_error("cache"); }); });
    check(f.snapshot() == before, "failed hole retirement retains old compensation view");
    check(f.capacity.repair_block(b, [] {}) == CapacityResult::Retired, "failed compensation target retires its owner");
    check(f.replacement.retired(physical), "record actual failed physical target rather than nominal default");
    check(f.capacity.holes().size(0) == 0, "retirement removes owner hole entry");
    check(f.capacity.retired_blocks().size() == 2 && f.capacity.retired(a), "only newly failed owner loses capacity");
    check(f.swap(1,2) == RemapResult::Applied, "later swap coordinates both permanent retirements");
    f.capacity.validate_invariants(true);
}
void capacity_lease_boundary() {
    Fixture f({2,1,1,8,20,0},100); // Two holes can be needed after a chained swap.
    f.retire({0,0,0,0}); check(f.swap(0,1) == RemapResult::Applied, "initial compensation");
    const LogicalBlockKey b{0,0,0,4}; const auto physical = f.target(b);
    auto read = f.resolver.pin(f.address(b));
    check(f.swap(0,2) == RemapResult::Applied, "outside owner remains usable during unrelated pair swap");
    check(f.target(b) == physical && block_of(read->physical()) == physical, "outside borrowed target is untouched");
    f.prepare(1,2); const auto before = f.snapshot();
    check(f.capacity.commit_swap(0,1,2,[] {}) == RemapResult::InFlight, "participating logical zone read pins transaction");
    check(f.snapshot() == before, "in-flight refusal preserves state");
    read.reset();
    const auto result = f.capacity.commit_swap(0,1,2,[] {});
    check(result == RemapResult::Applied || result == RemapResult::DensityLimit, "drained pair either commits or hits explicit density budget");
    if (result != RemapResult::Applied) f.capacity.cancel_quiesce(0,1,2);
    f.capacity.validate_invariants(true);
    Fixture pending;
    pending.capacity.mark_unusable(pending.target({0,0,0,0}));
    check(pending.swap(0,1) == RemapResult::RecoveryRequired, "swap waits for known failed usable owner repair");
    check(pending.capacity.repair_block({0,0,0,0},[] {}) == CapacityResult::InFlight, "repair cannot cross active quiesce");
    pending.capacity.cancel_quiesce(0,0,1);
    check(pending.capacity.repair_block({0,0,0,0},[] {}) == CapacityResult::Retired, "repair after cancel resolves owner");
    check(pending.swap(0,1) == RemapResult::Applied, "retry swap after recovery");
}
void capacity_chain() {
    Fixture f({2,2,2,8,20,0},250);
    for (auto k : std::vector<LogicalBlockKey>{{0,0,0,0},{0,0,0,1},{0,1,0,2},{0,0,0,4},
            {0,0,1,5},{0,1,1,8},{0,1,0,12},{0,0,1,16}}) f.retire(k);
    const auto retired = f.capacity.retired_blocks();
    std::uint64_t token = 1;
    for (auto k : f.keys()) if (!f.capacity.retired(k)) {
        PageData data{}; data.fill(DataToken{token++}); f.media.program(f.resolver.inspect(f.address(k)),data);
    }
    std::mt19937 random(20260925); std::size_t applied = 0;
    for (int step = 0; step < 400; ++step) {
        const auto a = random() % 5; auto b = random() % 4; if (b >= a) ++b;
        std::map<LogicalBlockKey, PhysicalBlockKey> outside;
        std::map<LogicalBlockKey, DataToken> data;
        std::set<PhysicalBlockKey> pool;
        std::vector<std::shared_ptr<ResolvedAddressLease>> leases;
        for (auto k : f.keys()) if (!f.capacity.retired(k)) {
            const auto p = f.target(k);
            if (k.channel != 0 || (k.block / f.groups != a && k.block / f.groups != b)) {
                outside.emplace(k,p); data.emplace(k,f.media.read(f.resolver.inspect(f.address(k)))->at(0));
                if (leases.size() < 3) leases.push_back(f.resolver.pin(f.address(k)));
            } else pool.insert(p);
        }
        f.prepare(a,b); const auto before = f.snapshot(); bool invalidated = false;
        const auto result = f.capacity.commit_swap(0,a,b,[&] { invalidated = true; });
        if (result == RemapResult::DensityLimit) {
            check(!invalidated && f.snapshot() == before, "random density refusal is atomic");
            f.capacity.cancel_quiesce(0,a,b);
        } else {
            check(result == RemapResult::Applied && invalidated, "random pair commits"); ++applied;
            std::set<PhysicalBlockKey> after;
            for (auto k : f.keys()) if (!f.capacity.retired(k) && k.channel == 0 &&
                    (k.block / f.groups == a || k.block / f.groups == b)) after.insert(f.target(k));
            check(pool == after, "pair conserves its actual healthy physical ownership set");
        }
        for (const auto& e : outside) {
            check(f.target(e.first) == e.second, "outside logical owner remains mapped to same physical block");
            check(f.media.read(f.resolver.inspect(f.address(e.first)))->at(0) == data.at(e.first), "outside owner data unchanged");
        }
        check(f.capacity.retired_blocks() == retired, "repeated swaps never revive or additionally retire logical blocks");
        f.capacity.validate_invariants(true);
    }
    check(applied > 100, "stress test actually exercises many committed swaps");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("scenario required"); const std::string s=argv[1];
        if (s == "capacity_compensation") capacity_compensation();
        else if (s == "capacity_density") capacity_density();
        else if (s == "capacity_atomicity") capacity_atomicity();
        else if (s == "capacity_hole_failure") capacity_hole_failure();
        else if (s == "capacity_lease_boundary") capacity_lease_boundary();
        else if (s == "capacity_chain") capacity_chain();
        else throw std::invalid_argument("unknown scenario");
        std::cout << "PASS " << s << " (" << checks << " checks)\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
