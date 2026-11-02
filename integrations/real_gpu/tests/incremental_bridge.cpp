#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include "hbflab/backend.hpp"
#include <iostream>
#include <string>
using namespace hbflab;
using namespace hbf_eventlab;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F fn) {
  bool threw = false;
  try {
    fn();
  } catch (const std::exception &) {
    threw = true;
  }
  check(threw, "expected rejection");
}
SimulationConfiguration config() {
  SimulationConfiguration c;
  c.device.geometry = {1, 1, 2, 16, 64, 1};
  c.device.capabilities = {256, 4, false, true};
  c.device.selected = {256, Tick{10000000},
                       c.device.geometry.block_group_dlus()};
  c.host.writes = WriteCommandMode::Burst4096;
  c.host.hints = ReadHintMode::AllRegular;
  c.host.max_resident_fragments = 256;
  return c;
}
void accounting(const TimingBackend &b) {
  const auto s = b.statistics();
  check(s.registered_globals == s.completed_globals,
        "every Global has one final result");
  check(s.registered_fragments == s.host.logical_success &&
            !s.host.logical_failed,
        "Host validates every logical fragment");
  check(s.device.accepted == s.device.delivered && b.drained(),
        "Host/Device transient work drained");
}
} // namespace
int main(int argc, char **argv) try {
  check(argc == 2, "case required");
  const std::string name = argv[1];
  if (name == "immutable" || name == "same_tick") {
    TensorAccessBridge reference(resolve_configuration(config()),
                                 {{"weights", GlobalByteAddress{0}, 8192, 1}});
    TimingBackend frontend;
    check(reference.initialize("weights").success, "upstream initialization");
    frontend.reserve(8192);
    for (unsigned i = 0; i < 4; ++i) {
      const auto gap = name == "same_tick" ? 0u : 123000u;
      const auto a = reference.access("weights", i * 64, 64, Tick{gap});
      const auto b = frontend.read(i * 64, 64, gap);
      check(a.success && a.complete && a.completed.has_value() &&
                b.status == Status::Ok,
            "both bridges succeed");
      check(a.completed->value - a.offered.value == b.end_ps - b.begin_ps,
            "same initialized media service latency");
    }
    const auto r = reference.finish();
    check(r.run.status == RunStatus::Completed && r.run.workload_success &&
              r.run.host.drained() && r.run.device.drained(),
          "upstream finish");
    accounting(frontend);
    frontend.finish();
    check(frontend.statistics().input_closed, "frontend final close");
  } else if (name == "backpressure") {
    BackendOptions o;
    o.resident_fragments = 64;
    TimingBackend b(o);
    b.reserve(8192);
    std::vector<TimedRead> inputs(12, {b.now_ps() + 10, {{0, 4096}}});
    const auto result = b.read_batch(inputs);
    check(result.size() == 12,
          "all parents retained across capacity rejection");
    for (const auto &r : result)
      check(r.status == Status::Ok && r.sectors == 64,
            "all rejected suffixes resumed");
    const auto s = b.statistics();
    check(s.backpressure > 0 && s.peak_resident_fragments == 64,
          "finite active Host input budget exercised");
    accounting(b);
  } else if (name == "block_version") {
    TimingBackend b;
    b.reserve(16384);
    std::array<std::uint64_t, 64> first{}, second{};
    first.fill(123);
    second.fill(456);
    check(b.write_page(0, first).status == Status::Ok, "first block version");
    check(b.write_page(8192, second).status == Status::Ok,
          "second block version includes unchanged pages");
    for (unsigned page = 0; page < 4; ++page)
      check(b.read(page * 4096, 4096).status == Status::Ok,
            "current whole-block content visible");
    check(b.statistics().writebacks == 2 && b.statistics().replayed_pages == 4,
          "two-page logical snapshots counted");
    accounting(b);
  } else if (name == "fault_guard") {
    SimulationSession session(resolve_configuration(config()));
    rejects([&] { session.set_program_outcome({}); });
    session.set_program_outcome(
        [](const ProgramOperation &) { return ProgramOutcome::Success; });
    session.advance_before(Tick{10});
    rejects([&] {
      session.set_program_outcome([](const ProgramOperation &) {
        return ProgramOutcome::FailedReusable;
      });
    });
    session.close_input();
    check(session.run_to_completion().run.status == RunStatus::Completed,
          "guarded session closes");
  } else if (name == "empty_tail") {
    TimingBackend b;
    b.reserve(4096);
    const auto before = b.now_ps();
    b.read_batch({{before + 123456, {}}});
    check(b.now_ps() == before + 123456,
          "compute-only producer contribution advances time");
    check(b.statistics().requests == 0,
          "compute tail creates no dummy memory request");
    accounting(b);
  } else if (name == "empty_same_tick") {
    TimingBackend b;
    b.reserve(4096);
    check(b.read(0, 64).status == Status::Ok, "first same-Tick read");
    const auto at = b.now_ps();
    b.read_batch({{at, {}}});
    check(b.now_ps() == at, "zero compute adds no dummy Tick");
    check(b.read(64, 64).status == Status::Ok,
          "causal handoff survives empty parent");
    accounting(b);
  } else if (name == "failure_drain") {
    BackendOptions o;
    o.fail_program_after = 4;
    TimingBackend b(o);
    b.reserve(16384);
    const auto initial = b.statistics().registered_globals;
    b.begin_stream(3);
    const auto at = b.now_ps() + 10;
    b.submit_write(1, at, 0, {});
    b.submit_read(2, {at, {{4096, 4096}}});
    b.submit_read(3, {at, {{8192, 4096}}});
    rejects([&] {
      while (b.advance_before(UINT64_MAX)) {
      }
    });
    const auto s = b.statistics();
    check(s.registered_globals > initial + 1 &&
              s.registered_globals == s.completed_globals,
          "normal final failure drains every registered Global");
    check(s.host.logical_failed > 0 && s.input_closed && !b.drained(),
          "logical failure closes input and poisons dependent execution");
    rejects([&] { b.take_completed(); });
  } else if (name == "partial_initialization") {
    BackendOptions o;
    o.fail_program_after = 1;
    TimingBackend b(o);
    rejects([&] { b.reserve(12288); });
    const auto s = b.statistics();
    check(s.writebacks == 0 && s.replayed_pages == 0,
          "partial initialization is never counted as application writeback");
    check(s.input_closed && s.registered_globals == s.completed_globals,
          "partial initialization drains its registered prefix");
    rejects([&] { b.read(0, 64); });
  } else if (name == "close") {
    TimingBackend b;
    b.reserve(4096);
    b.finish();
    b.finish();
    check(b.statistics().input_closed && b.drained(), "idempotent final close");
    rejects([&] { b.read(0, 64); });
  } else
    throw std::invalid_argument("unknown case");
  std::cout << "PASS incremental " << name << '\n';
  return 0;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 1;
}
