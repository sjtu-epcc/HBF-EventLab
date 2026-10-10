#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/runtime/zone_mapping.hpp"
#include "hbf_eventlab/runtime/write_assembly.hpp"
#include "hbf_eventlab/runtime/block_write_order.hpp"
#include "hbf_eventlab/policies/retained_assembly_storage.hpp"
#include "hbf_eventlab/infrastructure/event_queue.hpp"
#include "hbf_eventlab/policies/read_dispatch.hpp"
#include <iostream>
#include <string>
#include <set>

using namespace hbf_eventlab;
namespace {
std::size_t checks = 0;
void check(bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F f) {
    bool threw = false;
    try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, "expected rejection");
}
HostCommand command(std::uint64_t id, std::uint64_t dlu = 0, std::uint32_t bytes = 64) {
    return {CommandId{id}, Op::Write, {0, LocalByteAddress{dlu * dlu_bytes}}, bytes, 0};
}
void address_mapping() {
    const auto d = demonstration_profile();
    const auto& g = d.geometry();
    ReferenceAddressLayout layout(g);
    RoundRobinInterleaver host(GlobalByteAddress{4096}, g.host_channels, g.channel_bytes());
    check(host.map(GlobalByteAddress{4096 + 64}).channel == 1, "host channel striping");
    check(host.map(GlobalByteAddress{4096 + 128}).local.value == 64, "local byte units");
    rejects([&] { host.map(GlobalByteAddress{0}); });
    ZoneMapping zones(d);
    auto lease = zones.pin({0, LocalByteAddress{0}});
    zones.declare_discardable(0, 0); zones.declare_discardable(0, 1);
    check(zones.quiesce_pair(0, 0, 1), "quiesce pair");
    check(!zones.pin({0, LocalByteAddress{0}}), "quiesced zone must stop new IO");
    bool invalidated = false;
    check(zones.commit_swap(0, 0, 1, [&] { invalidated = true; }) == RemapResult::InFlight, "cannot remap active target");
    check(!invalidated, "must not invalidate before drain");
    lease.reset();
    check(zones.commit_swap(0, 0, 1, [&] { invalidated = true; }) == RemapResult::Applied, "swap after drain");
    zones.declare_discardable(0, 2);
    check(zones.quiesce_pair(0, 0, 2), "second pair");
    check(zones.commit_swap(0, 0, 2, [] {}) == RemapResult::Applied, "second swap");
    check(zones.mapped_zone(0, 0) == 2 && zones.mapped_zone(0, 1) == 0 && zones.mapped_zone(0, 2) == 1,
          "swap CURRENT mappings must not alias");
    check(zones.generation(1).value == 0 && zones.mapped_zone(1, 0) == 0, "channel isolation");
    std::set<std::uint64_t> targets;
    for (std::uint64_t i = 0; i < g.channel_dlus() / d.configuration().zone_size_dlus; ++i)
        check(targets.insert(zones.mapped_zone(0, i)).second, "zone mapping injective");
    for (std::uint64_t i = 0; i < g.channel_dlus(); ++i) {
        ChannelAddress a{0, LocalByteAddress{i * dlu_bytes + 64}};
        const auto logical = layout.logical_view(a);
        const auto physical = layout.decode(zones.translate(a));
        check(layout.encode_logical(logical) == a.local, "logical encode/decode roundtrip");
        check(logical.page == physical.page && logical.block.bank == physical.bank && logical.block.die == physical.die,
              "full-group remap must preserve page and lane");
    }
    check(zones.quiesce_pair(0, 0, 1), "throwing invalidation setup");
    rejects([&] { zones.commit_swap(0, 0, 1, [] { throw std::runtime_error("cache refused"); }); });
    check(zones.mapped_zone(0, 0) == 2 && zones.generation(0).value == 2, "failed invalidation must not commit swap");
    zones.cancel_quiesce(0, 0, 1);
    auto use = zones.pin({0, LocalByteAddress{0}}); use.reset();
    zones.quiesce_pair(0, 0, 1);
    check(zones.commit_swap(0, 0, 1, [] {}) == RemapResult::NotDiscardable, "new IO consumes Host discard declaration");
    zones.cancel_quiesce(0, 0, 1);
    auto bad = d.configuration(); bad.zone_size_dlus = g.pages_per_block;
    rejects([&] { StaticDeviceDescription invalid(g, d.capabilities(), bad); });
    rejects([&] { layout.decode({0, MediaDlu{g.channel_dlus()}, 0}); });
}
void admission() {
    const auto d = demonstration_profile();
    EventQueue events; AdmissionWaitRegistry signals(events);
    CommandAdmission ledger(2, 2, signals); // Small component-test budget, not a BUCC profile.
    check(ledger.try_accept(command(1)) == SubmitResult::Accepted, "first accept");
    check(ledger.try_accept(command(2)) == SubmitResult::Accepted, "second accept");
    check(ledger.try_accept(command(3)) == SubmitResult::Backpressured, "MOCS full is NOT protocol failure");
    check(ledger.outstanding(0) == 2 && ledger.outstanding(1) == 0, "no mutation on backpressure");
    rejects([&] { ledger.try_accept(command(1)); });
    ledger.response_delivered(0, CommandId{1});
    check(ledger.try_accept(command(3)) == SubmitResult::Accepted, "retry after credit return");
    rejects([&] { ledger.response_delivered(0, CommandId{1}); });
    CommandAdmission strict(1, 1, signals, AdmissionMode::StrictInputValidation);
    strict.try_accept(command(1));
    rejects([&] { strict.try_accept(command(2)); });
    check(strict.outstanding(0) == 1, "strict failure must not consume credit");
    auto burst = normalize_command(command(10, 0, dlu_bytes), d);
    check(burst.size() == 64 && burst.back().parent == CommandId{10} && burst.back().slot == 63, "burst identity preserved");
    auto crossing = command(20, 0, 128); crossing.op = Op::Read; crossing.address.local = LocalByteAddress{4032};
    rejects([&] { normalize_command(crossing, d); });
    auto no_burst = d.capabilities(); no_burst.write_burst = false;
    StaticDeviceDescription narrow(d.geometry(), no_burst, d.configuration());
    rejects([&] { normalize_command(command(1, 0, dlu_bytes), narrow); });
    auto cfg = d.configuration(); cfg.outstanding_limit = 1;
    rejects([&] { StaticDeviceDescription invalid(d.geometry(), no_burst, cfg); });
    StaticDeviceDescription single_burst(d.geometry(), d.capabilities(), cfg);
    check(single_burst.configuration().outstanding_limit == 1, "single-credit full-burst model can progress");
}
void write_lifecycle() {
    const auto d = demonstration_profile();
    RetainedAssemblyStorage store(d.geometry().host_channels * d.capabilities().max_pending_write_dlus);
    WriteAssembly writes(d, store);
    EventQueue events; AdmissionWaitRegistry signals(events);
    CommandAdmission ledger(2, 256, signals);
    auto cmd = command(1, 0, dlu_bytes);
    const auto fs = normalize_command(cmd, d);
    ledger.try_accept(cmd);
    auto first = writes.accept(fs[0], DataToken{100}, Tick{0});
    check(writes.forward(fs[0].key, 0).data == DataToken{100}, "partial forwarding");
    check(writes.forward(fs[0].key, 1).kind == ForwardKind::MissingFragment, "partial hole cannot fall back to old NAND");
    check(writes.accept(fs[0], DataToken{999}, Tick{0}).status == WriteStatus::OverlapAddress, "duplicate slot");
    check(writes.forward(fs[0].key, 0).data == DataToken{100}, "duplicate cannot overwrite original token");
    for (std::size_t i = 1; i < fs.size(); ++i) writes.accept(fs[i], DataToken{100 + i}, Tick{0});
    check(writes.find(fs[0].key)->awaiting_completion.size() == 1, "burst completes once");
    check(!writes.expire(fs[0].key, first.generation, Tick{100}), "Ready no longer has assembly timeout");
    writes.queue_program(fs[0].key, first.generation);
    writes.start_program(fs[0].key, first.generation);
    check(writes.forward(fs[0].key, 63).data == DataToken{163}, "data retained while Programming");
    rejects([&] { writes.commit_program(fs[0].key, first.generation, [](const PageData&) { throw std::runtime_error("sink failure"); }); });
    check(store.used() == 1 && writes.pending(0) == 1, "throwing sink retains owner");
    PageData media{};
    auto done = writes.commit_program(fs[0].key, first.generation, [&](const PageData& p) { media = p; });
    check(media[63] == DataToken{163} && store.used() == 0 && writes.pending(0) == 0, "commit before release");
    check(ledger.outstanding(0) == 1, "program completion is not Host response");
    ledger.response_delivered(0, done.commands.front());
    check(ledger.outstanding(0) == 0, "credit returned on Host delivery");
    for (std::uint64_t i = 0; i < 4; ++i) writes.accept(normalize_command(command(20+i, i), d)[0], DataToken{i}, Tick{10});
    check(writes.accept(normalize_command(command(30, 4), d)[0], DataToken{}, Tick{10}).status == WriteStatus::PendingDluLimit,
          "new DLU at limit returns 0x4");
    auto continuation = normalize_command(command(40, 0, dlu_bytes), d)[1];
    check(writes.accept(continuation, DataToken{1}, Tick{11}).status == WriteStatus::Success, "existing DLU can continue at limit");
    const auto old_gen = writes.find({0, LocalDlu{0}})->generation;
    auto timeout = writes.expire({0, LocalDlu{0}}, old_gen, Tick{110});
    check(timeout && timeout->status == WriteStatus::AssemblyTimeout && timeout->commands.size() == 2, "timeout completes all associated commands");
    const auto newer = writes.accept(normalize_command(command(50), d)[0], DataToken{50}, Tick{111});
    check(!writes.expire({0, LocalDlu{0}}, old_gen, Tick{211}), "old timeout cannot kill new generation");
    check(writes.find({0, LocalDlu{0}})->generation == newer.generation, "new generation remains");
    BlockWriteOrder order(8);
    order.start_page(0);
    check(!order.can_start(1), "next page cannot start before prior assembly");
    order.assembly_complete(0); order.start_program(0);
    order.start_page(1); order.assembly_complete(1); order.start_page(2);
    check(order.next_page_to_assemble() == 2 && order.next_page_to_program() == 0, "independent cursors");
    check(!order.can_program(1), "physical order waits for prior success");
    const auto failed = order.program_failure(0);
    check(failed.following_pages == std::vector<PageId>{1,2}, "failure identifies ready and collecting followers");
    check(!order.can_start(0) && order.mode() == BlockMode::AwaitingReplay, "freeze after program failure");
    order.restart_from_page0();
    check(order.can_start(0) && order.replay_through() == std::optional<PageId>{0}, "replay prefix ends at failed page; no block epoch");
    order.start_page(0); order.assembly_timeout(0);
    check(order.can_start(0), "assembly timeout allows same-page retry");
}
void events_dispatch() {
    EventQueue events;
    std::vector<int> sequence;
    events.schedule_at(Tick{10}, EventPhase::Deadline, [&] { sequence.push_back(3); });
    events.schedule_at(Tick{10}, EventPhase::Arrival, [&] { sequence.push_back(2); });
    events.schedule_at(Tick{10}, EventPhase::Completion, [&] { sequence.push_back(1); });
    const auto cancelled = events.schedule_at(Tick{99}, EventPhase::Arrival, [&] { sequence.push_back(9); });
    check(events.cancel(cancelled) && !events.cancel(cancelled), "event cancellation");
    events.run();
    check(sequence == std::vector<int>{1,2,3} && events.now() == Tick{10}, "tie ordering and no cancelled-time advance");
    rejects([&] { events.schedule_at(Tick{9}, EventPhase::Arrival, [] {}); });
    events.schedule_at(Tick{11}, EventPhase::Arrival, [&] { rejects([&] { events.run_next(); }); throw std::runtime_error("callback failure"); });
    rejects([&] { events.run_next(); });
    check(events.now() == Tick{11} && events.pending() == 0, "throwing callback consumed once");
    events.schedule_at(Tick{12}, EventPhase::Arrival, [] {});
    check(events.run_next(), "engine reusable after exception");
    events.schedule_at(Tick{12}, EventPhase::Completion, [] {});
    check(events.run_next() && events.round()==1,"earlier phase uses next causal round");
    WindowDispatchPolicy timer(Tick{10}); ReadDispatchWindow window(timer);
    check(window.arrive(CommandId{1}, ReadHint::Regular, Tick{0}).empty(), "regular accumulation");
    window.arrive(CommandId{2}, ReadHint::Regular, Tick{1});
    auto b1 = window.arrive(CommandId{3}, ReadHint::Batch, Tick{2});
    auto b2 = window.arrive(CommandId{4}, ReadHint::Batch, Tick{3});
    check(b1.size() == 1 && b1[0].size() == 3 && b2.size() == 1 && b2[0].size() == 1, "R R B B immediate batch interpretation");
    window.arrive(CommandId{5}, ReadHint::Regular, Tick{4});
    check(!window.flush_due(Tick{10}), "stale old-window wakeup cannot flush early");
    check(window.next_wakeup() == std::optional<Tick>{Tick{14}} && window.flush_due(Tick{14})->size() == 1, "regular timer progresses without arrivals");
    WindowDispatchPolicy depth(Tick{20}, 2); ReadDispatchWindow threshold(depth);
    threshold.arrive(CommandId{1}, ReadHint::Regular, Tick{0});
    check(threshold.arrive(CommandId{2}, ReadHint::Regular, Tick{1}).size() == 1, "replaceable depth policy");
    const auto d = demonstration_profile();
    RetainedAssemblyStorage store(8); WriteAssembly writes(d, store);
    const auto fs = normalize_command(command(77, 0, dlu_bytes), d);
    const auto first = writes.accept(fs[0], DataToken{0}, Tick{0});
    EventQueue deadline_events;
    deadline_events.schedule_at(first.deadline, EventPhase::Deadline, [&] {
        check(!writes.expire(fs[0].key, first.generation, deadline_events.now()), "deadline tie: complete DLU must not expire");
    });
    deadline_events.schedule_at(first.deadline, EventPhase::Arrival, [&] {
        for (std::size_t i = 1; i < fs.size(); ++i) writes.accept(fs[i], DataToken{i}, deadline_events.now());
    });
    deadline_events.run();
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("scenario required");
        const std::string scenario = argv[1];
        if (scenario == "address_mapping") address_mapping();
        else if (scenario == "admission") admission();
        else if (scenario == "write_lifecycle") write_lifecycle();
        else if (scenario == "events_dispatch") events_dispatch();
        else throw std::invalid_argument("unknown scenario");
        std::cout << "PASS " << scenario << " (" << checks << " checks)\n";
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
