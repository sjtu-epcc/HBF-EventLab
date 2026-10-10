#include "hbf_eventlab/runtime/write_path.hpp"
#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include <iostream>
#include <string>

using namespace hbf_eventlab;
namespace {
std::size_t checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool threw = false; try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, "expected rejection");
}
StaticDeviceDescription profile(std::uint32_t pending = 8, std::uint32_t spares = 2, std::uint32_t mocs = 256) {
    Geometry g{2, 2, 2, 8, 3, spares};
    return {g, {256, pending, true, true}, {mocs, Tick{100}, g.block_group_dlus()}};
}
struct Fixture {
    StaticDeviceDescription d;
    ReferenceAddressLayout layout;
    ZoneMapping zones;
    BlockReplacement replacement;
    NandMediaState media;
    CapacityRetirementCoordinator capacity;
    AddressResolver resolver;
    EventQueue events;
    AdmissionWaitRegistry signals{events};
    CommandAdmission admission;
    WritePath writes;
    LogicalBlockKey b{0, 0, 0, 0};
    std::uint64_t id = 0;
    explicit Fixture(std::uint32_t pending = 8, std::uint32_t spares = 2, std::uint32_t mocs = 256, std::uint32_t permille = 50)
        : d(profile(pending, spares, mocs)), layout(d.geometry()), zones(d), replacement(d.geometry()),
          media(d.geometry()), capacity(d, layout, zones, replacement, media, CapacityPolicy{permille}), resolver(capacity),
          admission(d.geometry().host_channels, d.configuration().outstanding_limit,signals),
          writes(d, layout, capacity, media, events, admission) {}
    ChannelAddress address(PageId p, std::uint32_t slot = 0) const {
        return {b.channel, layout.encode_logical({b, p, slot * host_unit_bytes})};
    }
    DluKey key(PageId p) const { return {b.channel, LocalDlu{address(p).local.value / dlu_bytes}}; }
    WriteSubmission send(PageId p, std::uint32_t slot = 0, std::uint32_t count = 64, std::uint64_t token = 10) {
        HostCommand cmd{CommandId{++id}, Op::Write, address(p, slot), count * host_unit_bytes, 0};
        return writes.submit(cmd, std::vector<DataToken>(count, DataToken{token}));
    }
    void good(PageId p, std::uint64_t token = 10) {
        const auto s = send(p, 0, 64, token);
        check(s.admission == SubmitResult::Accepted && !s.immediate_status, "write accepted");
    }
    void erase() { for (auto op : writes.pending_erases()) writes.finish_erase(writes.issue_erase(op.id)->id); }
    ProgramOperation start(PageId p) {
        auto op = writes.issue_program(*writes.program_candidate(key(p))); check(op.has_value(), "program starts"); return *op;
    }
    void finish(PageId p, ProgramOutcome result = ProgramOutcome::Success) { writes.finish_program(start(p).id, result); }
    void deliver() { for (const auto& r : writes.responses()) writes.response_delivered(r.channel, r.command); }
};
void replacement_pipeline() {
    Fixture f;
    check(to_bytes(LocalAddress64B{64}) == LocalByteAddress{4096}, "64B unit conversion");
    check(to_units64(LocalByteAddress{4096}) == LocalAddress64B{64}, "byte offset conversion");
    rejects([] { to_units64(LocalByteAddress{1}); });
    const auto a = f.address(2, 3);
    const auto slot = f.resolver.slot(a);
    auto read = f.resolver.pin(a);
    f.capacity.mark_unusable(block_of(read->physical()));
    check(f.capacity.repair_block(f.b, [] {}) == CapacityResult::InFlight, "replacement waits for pinned read");
    read.reset();
    rejects([&] { f.capacity.repair_block(f.b, [] { throw std::runtime_error("cache"); }); });
    check(f.resolver.inspect(a).block == 0, "failed invalidation cannot change mapping");
    check(f.capacity.repair_block(f.b, [] {}) == CapacityResult::Replaced, "spare selected");
    const auto replaced = f.resolver.inspect(a);
    check(replaced.block == 3 && replaced.page == 2 && replaced.byte_offset == 192, "replacement preserves page/offset");
    check(replaced.bank == slot.bank && replaced.die == slot.die, "replacement stays in same bank/die");
    check(f.replacement.retired({0, 0, 0, 0}), "failed physical target retired");
    check(f.zones.mapped_zone(0, 0) == 0, "block replacement does not swap zones");
    const auto peer = f.layout.encode_logical({{0, 0, 1, 0}, 2, 0});
    check(f.resolver.inspect({0, peer}).block == 0, "healthy lane unchanged");
    f.capacity.declare_discardable(0, 0); f.capacity.declare_discardable(0, 1);
    f.capacity.quiesce_pair(0, 0, 1);
    check(f.capacity.commit_swap(0, 0, 1, [] {}) == RemapResult::Applied, "zone swap after replacement");
    check(f.resolver.inspect(a).block == 1, "new zone points to its own slot");
    const auto other = f.layout.encode_logical({{0, 0, 0, 1}, 2, 192});
    check(f.resolver.inspect({0, other}).block == 3, "replacement follows media slot across zone swap");
    const auto generation = f.zones.generation(0);
    auto combined = f.resolver.pin(a);
    f.capacity.declare_discardable(0, 0); f.capacity.declare_discardable(0, 1);
    f.capacity.quiesce_pair(0, 0, 1);
    check(f.capacity.commit_swap(0, 0, 1, [] {}) == RemapResult::InFlight, "combined lease pins zone too");
    combined.reset(); f.capacity.cancel_quiesce(0, 0, 1);
    check(f.zones.generation(0) == generation, "blocked swap no update");
    Fixture g;
    g.good(0); g.erase(); g.finish(0); g.deliver();
    g.good(1); g.finish(1); g.deliver();
    g.capacity.declare_discardable(0, 0); g.capacity.declare_discardable(0, 1); g.capacity.quiesce_pair(0, 0, 1);
    check(g.capacity.commit_swap(0, 0, 1, [] {}) == RemapResult::Applied, "remap after complete writes");
    check(g.send(2).immediate_status == WriteStatus::WriteOrderViolation, "old cursor cannot continue on remapped target");
    g.deliver(); g.good(0); g.erase(); g.finish(0); g.deliver();
    check(g.resolver.inspect(g.address(0)).block == 1, "new start after swap uses new physical block");
    check(g.media.erase_count({0, 0, 0, 0}) == 1 && g.media.erase_count({0, 0, 0, 1}) == 1, "zone swap does not transfer/reset wear");
}
void overlap_restart() {
    Fixture f;
    check(f.send(0, 7, 1, 77).disposition == WriteDisposition::BeginBlock, "first page-0 fragment opens one preparation");
    check(f.writes.pending_erases().size() == 1, "one erase ticket");
    check(f.send(0, 7, 1, 99).immediate_status == WriteStatus::OverlapAddress, "duplicate 64B overlap");
    check(f.send(0, 0, 64, 99).immediate_status == WriteStatus::OverlapAddress, "burst overlap preflight");
    check(f.writes.find(f.key(0))->received.count() == 1, "rejected burst changes no earlier holes");
    check(f.writes.forward(f.key(0), 7).data == DataToken{77}, "duplicate preserves original data");
    f.deliver();
    for (std::uint32_t s = 0; s < 64; ++s) if (s != 7)
        check(f.send(0, s, 1, s).disposition == WriteDisposition::Supplement, "page-0 supplement");
    check(f.writes.pending_erases().size() == 1, "supplements never re-erase");
    check(!f.writes.issue_program(*f.writes.program_candidate(f.key(0))), "erase must complete before program");
    check(f.send(0).immediate_status == WriteStatus::OverlapAddress, "Ready overlap");
    f.deliver(); f.erase();
    const auto old = f.start(0);
    check(f.send(0).immediate_status == WriteStatus::OverlapAddress, "Programming overlap");
    f.deliver(); f.writes.finish_program(old.id);
    check(f.send(0).admission == SubmitResult::Backpressured, "program success still awaits old responses");
    f.deliver();
    f.good(1); f.finish(1); f.deliver();
    check(f.send(1).immediate_status == WriteStatus::OverlapAddress, "committed nonzero page overlap with no Pending entry");
    f.deliver();
    const auto physical = f.resolver.inspect(f.address(0));
    auto read = f.resolver.pin(f.address(0));
    check(f.send(0).admission == SubmitResult::Backpressured, "old read prevents erase boundary");
    read.reset();
    check(f.send(0, 0, 64, 300).disposition == WriteDisposition::BeginBlock, "ordinary overwrite needs no error/replay flag");
    f.erase(); f.finish(0); f.deliver();
    check(block_of(f.resolver.inspect(f.address(0))) == block_of(physical), "refresh/new content retains healthy target");
    check(f.media.erase_count(block_of(physical)) == 2, "one erase per actual block start");
    check(f.media.read(physical)->at(0) == DataToken{300}, "new content committed");
    rejects([&] { f.writes.finish_program(old.id); });
    check(f.writes.order(f.b).next_page_to_program() == 1, "old completion cannot alter new block write");
}
void timeout_retry() {
    Fixture f;
    f.send(0, 0, 1);
    const auto old_generation = f.writes.find(f.key(0))->generation;
    const auto erase = f.writes.pending_erases().front();
    f.events.run();
    check(f.writes.responses().front().status == WriteStatus::AssemblyTimeout, "collecting timeout");
    f.deliver();
    check(f.writes.preparation(f.b) == BlockPreparation::ErasePending, "timeout does not cancel actual erase");
    check(f.send(0, 1, 1).disposition == WriteDisposition::RetryPage0, "retry while erase pending");
    check(f.writes.find(f.key(0))->generation != old_generation, "DLU identity renewed without block epoch");
    check(f.writes.pending_erases().front().id == erase.id, "retry reuses erase ticket");
    f.writes.finish_erase(f.writes.issue_erase(erase.id)->id);
    f.events.run(); f.deliver();
    check(f.send(0).disposition == WriteDisposition::RetryPage0, "retry after erase complete");
    check(f.writes.pending_erases().empty(), "no second erase for page-0 retry");
    f.finish(0); f.deliver();
    check(f.media.erase_count(block_of(f.resolver.inspect(f.address(0)))) == 1, "retry leaves PEC unchanged");
    f.send(1, 0, 1); f.events.run(); f.deliver();
    check(f.send(1).disposition == WriteDisposition::NewPage, "nonzero-page retry does not replay block");
    f.finish(1); f.deliver();
    check(f.writes.order(f.b).next_page_to_program() == 2, "same-page timeout retry resumes cursor");
    Fixture teardown;
    {
        WritePath temporary(teardown.d, teardown.layout, teardown.capacity, teardown.media, teardown.events, teardown.admission);
        HostCommand cmd{CommandId{999}, Op::Write, f.address(0), 64, 0};
        temporary.submit(cmd, {DataToken{1}});
    }
    teardown.events.run(); check(teardown.events.now() == Tick{0}, "destroyed path leaves no live timeout callback");
    Fixture at_deadline;
    at_deadline.send(0, 0, 1); at_deadline.erase();
    at_deadline.events.schedule_at(Tick{100}, EventPhase::Arrival, [&] {
        for (std::uint32_t s = 1; s < 64; ++s) at_deadline.send(0, s, 1);
    });
    at_deadline.events.run();
    check(at_deadline.writes.responses().empty() && at_deadline.writes.find(at_deadline.key(0))->phase == WritePhase::Ready,
          "arrival at exact deadline completes before timeout in integrated path");
    at_deadline.finish(0); at_deadline.deliver();
}
void replay_prefix() {
    Fixture f;
    f.good(0); f.erase(); f.finish(0); f.deliver();
    f.good(1); f.finish(1); f.deliver();
    f.good(2); f.good(3); f.send(4, 0, 1);
    f.finish(2, ProgramOutcome::FailedReusable);
    const auto replies = f.writes.responses();
    check(replies.size() == 3, "failed and all accepted followers terminated");
    check(replies[0].status == WriteStatus::ProgramFail, "failed page 0x7");
    check(replies[1].status == WriteStatus::WriteOrderViolation && replies[1].additional_info == 2 &&
          replies[2].status == WriteStatus::WriteOrderViolation && replies[2].additional_info == 2, "followers 0x6 info2");
    check(f.writes.pending(0) == 0, "no old pages left to silently resume");
    check(f.send(0).admission == SubmitResult::Backpressured, "replay waits for error responses");
    f.deliver();
    check(f.send(3).immediate_status == WriteStatus::WriteOrderViolation, "cannot skip replay after failure");
    f.deliver();
    const auto original = f.resolver.inspect(f.address(0));
    f.good(0); f.erase();
    check(f.writes.order(f.b).replay_through() == std::optional<PageId>{2}, "replay endpoint is failed page, not block end");
    f.good(1); f.good(2);
    check(f.send(3).admission == SubmitResult::Backpressured, "follow-up waits for replay prefix completion");
    for (PageId p = 0; p <= 2; ++p) { f.finish(p); f.deliver(); }
    check(!f.writes.order(f.b).replay_through(), "failed prefix restored");
    check(!f.writes.find(f.key(3)), "old page-3 request is not resurrected");
    f.good(3); f.finish(3); f.deliver();
    check(block_of(f.resolver.inspect(f.address(0))) == block_of(original), "reusable failure keeps mapping");
    check(f.writes.order(f.b).next_page_to_program() == 4, "Host resubmission resumes at next page");
    Fixture restart;
    restart.good(0); restart.erase(); restart.finish(0); restart.deliver();
    restart.good(1); restart.finish(1); restart.deliver();
    restart.good(2); restart.finish(2, ProgramOutcome::FailedReusable); restart.deliver();
    restart.good(0); restart.erase(); restart.finish(0); restart.deliver();
    check(restart.send(0).disposition == WriteDisposition::BeginBlock, "drained partial replay permits another start");
    restart.erase(); restart.finish(0); restart.deliver();
    check(restart.writes.order(restart.b).replay_through() == std::optional<PageId>{2},
          "restarting a partial replay preserves the failed prefix obligation");
    restart.good(1); restart.good(2);
    check(restart.send(3).admission == SubmitResult::Backpressured, "repeated start cannot release follow-up pages early");
    restart.finish(1); restart.finish(2); restart.deliver();
    check(!restart.writes.order(restart.b).replay_through(), "repeated-start prefix retires only on failed-page success");
    restart.good(3); restart.finish(3); restart.deliver();
}
void replacement_failure() {
    Fixture f(8, 1);
    f.good(0); f.erase(); f.finish(0, ProgramOutcome::FailedUnusable); f.deliver();
    const auto before = f.resolver.inspect(f.address(0));
    f.good(0);
    const auto after = f.resolver.inspect(f.address(0));
    check(after.block == 3 && before.block == 0, "permanent failure gets same-bank spare");
    check(f.zones.mapped_zone(0, 0) == 0, "no zone swap for fault replacement");
    f.erase(); f.finish(0); f.deliver();
    check(f.media.erase_count(block_of(before)) == 1 && f.media.erase_count(block_of(after)) == 1, "wear belongs to each physical block");
    f.good(1); f.finish(1, ProgramOutcome::FailedUnusable); f.deliver();
    check(f.send(0).immediate_status == WriteStatus::CapacityUnusable, "no spare gives retired block error");
    f.deliver();
    check(f.send(0).immediate_status == WriteStatus::InvalidAddress, "later access to retired logical capacity is invalid");
    f.deliver();
    check(f.writes.pending_erases().empty() && f.writes.pending(0) == 0, "spare exhaustion starts no erase or DLU");
}
void replay_merge() {
    Fixture f;
    for (PageId p=0; p<5; ++p) { f.good(p); f.erase(); f.finish(p); f.deliver(); }
    f.good(5); f.good(6); f.send(7,0,1);
    const auto old = f.start(5);
    f.writes.finish_program(old.id, ProgramOutcome::FailedReusable);
    check(f.writes.responses().size() == 3, "initial failure terminates ready and collecting followers");
    f.deliver();
    check(f.writes.order(f.b).replay().required_through() == std::optional<PageId>{5}, "first debt ends at page five");
    f.good(0); f.erase(); f.finish(0); f.deliver();
    f.good(1); f.good(2); f.send(3,0,1);
    f.finish(1, ProgramOutcome::FailedReusable);
    check(f.writes.responses().size() == 3, "second failure terminates this attempt only");
    f.deliver();
    check(f.writes.order(f.b).replay_through() == std::optional<PageId>{5}, "earlier replay failure cannot shrink unfinished endpoint");
    check(f.writes.order(f.b).replay().last_failure() == std::optional<PageId>{1} &&
          f.writes.order(f.b).replay().failure_count() == 2, "diagnostics distinguish last failure from required endpoint");
    f.good(0); f.erase(); f.finish(0, ProgramOutcome::FailedReusable); f.deliver();
    check(f.writes.order(f.b).replay_through() == std::optional<PageId>{5}, "failure at page zero preserves farthest endpoint");
    f.good(0); f.erase(); f.finish(0); f.deliver();
    f.good(1); f.finish(1); f.deliver();
    f.send(2,0,1); f.events.run();
    check(f.writes.responses().size() == 1 && f.writes.responses().front().status == WriteStatus::AssemblyTimeout,
          "replay assembly timeout completes only current DLU");
    f.deliver();
    check(f.writes.order(f.b).replay_through() == std::optional<PageId>{5}, "timeout does not discharge replay debt");
    f.good(2); f.finish(2); f.deliver();
    for (PageId p=3; p<=5; ++p) f.good(p);
    check(f.send(6).admission == SubmitResult::Backpressured, "cannot pass merged endpoint before its program success");
    f.finish(3); f.finish(4); f.finish(5, ProgramOutcome::FailedReusable); f.deliver();
    check(f.writes.order(f.b).replay().failure_count() == 4, "equal-endpoint failure merges into same obligation");
    rejects([&] { f.writes.finish_program(old.id); });
    check(f.writes.order(f.b).replay_through() == std::optional<PageId>{5}, "stale completion cannot clear merged debt");
    for (PageId p=0; p<=5; ++p) { f.good(p,100+p); f.erase(); f.finish(p); f.deliver(); }
    check(f.writes.order(f.b).replay().phase() == ReplayPhase::Idle &&
          f.writes.order(f.b).replay().failure_count() == 0 && !f.writes.order(f.b).replay_through(),
          "only successful merged endpoint clears current recovery");
    check(!f.writes.find(f.key(6)), "old failed follower not resurrected by merge");
    f.good(6); f.finish(6,ProgramOutcome::FailedReusable); f.deliver();
    check(f.writes.order(f.b).replay_through() == std::optional<PageId>{6} &&
          f.writes.order(f.b).replay().failure_count() == 1, "later independent failure starts a fresh obligation");
}
void replay_replacement_merge() {
    Fixture f(8,2);
    for (PageId p=0; p<5; ++p) { f.good(p); f.erase(); f.finish(p); f.deliver(); }
    f.good(5); f.finish(5,ProgramOutcome::FailedReusable); f.deliver();
    for (PageId p=0; p<2; ++p) { f.good(p); f.erase(); f.finish(p); f.deliver(); }
    f.good(2); f.finish(2,ProgramOutcome::FailedUnusable); f.deliver();
    f.good(0); f.erase(); f.finish(0); f.deliver();
    check(f.resolver.inspect(f.address(0)).block == 3 &&
          f.writes.order(f.b).replay().target() == std::optional<PhysicalBlockKey>{{0,0,0,3}}, "replay target binds first spare");
    check(f.writes.order(f.b).replay_through() == std::optional<PageId>{5}, "replacement preserves logical recovery endpoint");
    f.good(1); f.finish(1,ProgramOutcome::FailedUnusable); f.deliver();
    f.good(0); f.erase();
    check(f.resolver.inspect(f.address(0)).block == 4 && f.writes.order(f.b).replay_through() == std::optional<PageId>{5},
          "second replacement still preserves farthest unfinished endpoint");
    f.finish(0,ProgramOutcome::FailedUnusable); f.deliver();
    check(f.send(0).immediate_status == WriteStatus::CapacityUnusable, "exhaustion reports newly retired capacity");
    f.deliver();
    check(f.writes.order(f.b).mode() == BlockMode::Retired &&
          f.writes.order(f.b).replay().phase() == ReplayPhase::Retired && !f.writes.order(f.b).replay_through(),
          "permanent retirement closes recovery without fabricating success");
    check(f.send(0).immediate_status == WriteStatus::InvalidAddress, "retired logical address cannot start new replay");
    f.deliver(); rejects([&] { f.resolver.pin(f.address(0)); });
    f.capacity.validate_invariants(true);
}
void replay_remap_generation() {
    Fixture f;
    f.good(0); f.erase(); f.finish(0); f.deliver();
    f.good(1); f.finish(1); f.deliver(); f.good(2); f.finish(2,ProgramOutcome::FailedReusable); f.deliver();
    const auto original = block_of(f.resolver.inspect(f.address(0)));
    for (int i=0; i<2; ++i) {
        f.capacity.declare_discardable(0,0); f.capacity.declare_discardable(0,1); f.capacity.quiesce_pair(0,0,1);
        check(f.capacity.commit_swap(0,0,1,[] {}) == RemapResult::Applied, "discarded contents can be remapped");
    }
    check(block_of(f.resolver.inspect(f.address(0))) == original, "ABA ends at identical physical coordinates");
    check(f.send(1).immediate_status == WriteStatus::WriteOrderViolation, "ABA does not reuse old write cursor");
    check(f.writes.responses().front().additional_info == 1, "discarded old recovery no longer controls new content");
    f.deliver(); f.good(0); f.erase(); f.finish(0); f.deliver();
    check(!f.writes.order(f.b).replay_through() && f.writes.order(f.b).replay().content_generation() == Generation{2},
          "explicit discard/remap starts fresh content generation");
    Fixture timeout;
    timeout.send(0,0,1); const auto erase = *timeout.writes.issue_erase(timeout.writes.pending_erases().front().id);
    timeout.events.run(); timeout.deliver();
    timeout.capacity.declare_discardable(0,0); timeout.capacity.declare_discardable(0,1); timeout.capacity.quiesce_pair(0,0,1);
    check(timeout.capacity.commit_swap(0,0,1,[] {}) == RemapResult::InFlight, "timed-out DLU still pins unfinished erase");
    timeout.writes.finish_erase(erase.id);
    check(timeout.capacity.commit_swap(0,0,1,[] {}) == RemapResult::Applied, "erase drain enables remap");
    timeout.capacity.declare_discardable(0,0); timeout.capacity.declare_discardable(0,1); timeout.capacity.quiesce_pair(0,0,1);
    check(timeout.capacity.commit_swap(0,0,1,[] {}) == RemapResult::Applied, "swap back after timeout");
    check(timeout.send(0).disposition == WriteDisposition::BeginBlock, "ABA invalidates timeout preparation even at same physical target");
    check(timeout.writes.pending_erases().size() == 1 && timeout.writes.active_erases() == 0,
          "new content gets new erase ticket");
    rejects([&] { timeout.writes.finish_erase(erase.id); });
    timeout.erase(); timeout.finish(0); timeout.deliver();
    check(timeout.media.erase_count({0,0,0,0}) == 2, "fresh erase is distinct from old timed-out preparation");
}
void retirement_write_swap() {
    Fixture f(8,0,256,1000);
    f.good(0); f.erase(); f.finish(0,ProgramOutcome::FailedUnusable); f.deliver();
    check(f.send(0).immediate_status == WriteStatus::CapacityUnusable, "initial logical retirement through WritePath");
    f.deliver();
    f.capacity.declare_discardable(0,0); f.capacity.declare_discardable(0,1); f.capacity.quiesce_pair(0,0,1);
    check(f.capacity.commit_swap(0,0,1,[] {}) == RemapResult::Applied, "swap compensates retired hole for still-usable B");
    check(f.send(0).immediate_status == WriteStatus::InvalidAddress, "shared retirement view survives zone swap");
    f.deliver();
    f.b.block=1; f.good(0); f.erase();
    const auto op=f.start(0);
    check(op.physical.block == 1, "write programs final compensation target rather than bad nominal slot");
    f.writes.finish_program(op.id); f.deliver();
    check(f.media.read(f.resolver.inspect(f.address(0)))->at(0) == DataToken{10}, "compensated write reaches healthy physical media");
    f.good(1); f.finish(1,ProgramOutcome::FailedUnusable); f.deliver();
    check(f.send(0).immediate_status == WriteStatus::CapacityUnusable, "failure of compensation target retires actual owner");
    f.deliver();
    check(f.capacity.retired_blocks().size() == 2 && f.capacity.holes().size(0) == 0, "hole and logical retirement updated together");
    f.capacity.validate_invariants(true);
}
void admission_boundaries() {
    Fixture f(1);
    f.send(0, 0, 1);
    const auto original = f.b;
    f.b.bank = 1;
    check(f.send(0).immediate_status == WriteStatus::PendingDluLimit, "new block at pending limit gives 0x4");
    check(f.writes.pending_erases().size() == 1, "rejected start does not erase other block");
    f.deliver(); f.b = original;
    check(f.send(0, 1, 1).disposition == WriteDisposition::Supplement, "pending limit still allows holes");
    Fixture g;
    g.good(0); g.erase(); g.finish(0); g.deliver();
    g.send(1, 0, 1);
    check(g.send(0).admission == SubmitResult::Backpressured, "restart deferred behind old collecting page");
    check(g.writes.outstanding(0) == 1, "deferred restart consumes no credit");
    for (std::uint32_t s = 1; s < 64; ++s) check(g.send(1, s, 1).disposition == WriteDisposition::Supplement, "old holes can progress");
    g.finish(1); g.deliver();
    check(g.send(0).disposition == WriteDisposition::BeginBlock, "restart after old page drains");
    Fixture h;
    h.send(0, 0, 1);
    h.capacity.declare_discardable(0, 0); h.capacity.declare_discardable(0, 1); h.capacity.quiesce_pair(0, 0, 1);
    check(h.send(0, 1, 1).disposition == WriteDisposition::Supplement, "existing context can drain quiescing zone");
    h.capacity.cancel_quiesce(0, 0, 1);
    Fixture credit(8, 2, 1);
    credit.good(0);
    credit.b.bank = 1;
    check(credit.send(0).admission == SubmitResult::Backpressured, "MOCS full: no second parent accepted");
    check(credit.writes.pending_erases().size() == 1 && credit.writes.pending(0) == 1, "MOCS backpressure creates no media work");
    credit.b.bank = 0; credit.erase(); credit.finish(0);
    check(credit.send(0).admission == SubmitResult::Backpressured, "credit retained after media completion");
    credit.deliver();
    check(credit.send(0).disposition == WriteDisposition::BeginBlock, "credit return opens next block write");
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("scenario required");
        const std::string s = argv[1];
        if (s == "replacement_pipeline") replacement_pipeline();
        else if (s == "overlap_restart") overlap_restart();
        else if (s == "timeout_retry") timeout_retry();
        else if (s == "replay_prefix") replay_prefix();
        else if (s == "replacement_failure") replacement_failure();
        else if (s == "admission_boundaries") admission_boundaries();
        else if (s == "replay_merge") replay_merge();
        else if (s == "replay_replacement_merge") replay_replacement_merge();
        else if (s == "replay_remap_generation") replay_remap_generation();
        else if (s == "retirement_write_swap") retirement_write_swap();
        else throw std::invalid_argument("unknown scenario");
        std::cout << "PASS " << s << " (" << checks << " checks)\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
