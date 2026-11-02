#include "hbflab/backend.hpp"
#include "hbflab/frontier.hpp"
#include <iostream>
#include <limits>
#include <string>

using namespace hbflab;
void check(bool b, const char* why) { if (!b) throw std::runtime_error(why); }
template<class F> void rejects(F f) {
    bool threw = false; try { f(); } catch (const std::exception&) { threw = true; }
    check(threw, "expected rejection");
}
int main(int argc, char** argv) {
    try {
        const std::string name = argc > 1 ? argv[1] : "";
        BackendOptions o;
        if (name == "frontier") {
            std::vector<Request> r(3);
            for (unsigned i = 0; i < r.size(); ++i) { r[i].producer = i; r[i].ticket = 5; r[i].arrival_ns = i ? 10 : 30; }
            const auto sorted = order_frontier(r, 5, 10, 100, 100, 2);
            check(sorted[0].producer == 1 && sorted[1].producer == 2 && sorted[2].producer == 0, "arrival and stable owner tie order");
            check(sorted[2].arrival_ps == 10100, "common epoch scaling");
            rejects([&] { order_frontier(r, 5, 10, 100, 101, 2); });
            rejects([&] { order_frontier(r, 6, 10, 100, 100, 2); });
            r[1].producer = 0;
            rejects([&] { order_frontier(r, 5, 10, 100, 100, 2); });
            check(scale_ticks(UINT64_MAX, 1000, 1000) == UINT64_MAX, "avoid intermediate overflow");
            check(scale_ticks(1001, 1, 1000, true) == 2, "round delay up");
            rejects([&] { scale_ticks(UINT64_MAX, 1000, 1); });
            rejects([&] { scale_ticks(1, 1, 0); });
        } else if (name == "batch_parallel") {
            o.timing.read_dispatch.threshold = 1; o.timing.read_dispatch.wait = hbf_eventlab::Tick{1};
            o.timing.media_timing.sense = hbf_eventlab::Tick{1000000};
            TimingBackend parallel(o), serial(o); parallel.reserve(12288); serial.reserve(12288);
            const auto start = parallel.now_ps();
            const auto p = parallel.read_batch({{start, {{0, 4}}}, {start, {{4096, 4}}}});
            const auto q = serial.read_batch({{serial.now_ps(), {{0, 4}}}, {serial.now_ps(), {{8192, 4}}}});
            check(p[0].status == Status::Ok && p[1].status == Status::Ok, "parallel success");
            const auto parallel_span = std::max(p[0].end_ps, p[1].end_ps) - start;
            const auto serial_span = std::max(q[0].end_ps, q[1].end_ps) - q[0].begin_ps;
            check(parallel_span < serial_span && parallel_span < 1500000, "different Banks overlap instead of host serialization");
            check(p[0].end_ps != p[1].end_ps, "individual completion times must not become the batch drain time");
            check(parallel.drained() && serial.drained(), "both batches drained");
            std::cout << "parallel_ps=" << parallel_span << " serial_ps=" << serial_span << '\n';
        } else if (name == "batch_arrivals") {
            TimingBackend b; b.reserve(8192);
            const auto at = b.now_ps();
            const auto r = b.read_batch({{at, {{0, 4}}}, {at + 10000000, {{4096, 4}}}});
            check(r[0].end_ps < r[1].begin_ps && r[1].end_ps > r[1].begin_ps, "future request cannot delay earlier completion");
            rejects([&] { b.read_batch({{b.now_ps()+1, {{0,4}}}, {b.now_ps(), {{4096,4}}}}); });
            rejects([&] { b.read_batch({{at, {{0,4}}}}); });
            check(b.statistics().requests == 2 && b.drained(), "late/unsorted requests rejected without effects");
        } else if (name == "batch_union") {
            o.outstanding = 1; o.timing.buffers.ingress = {1, 4096}; o.timing.buffers.egress = {1, 64};
            TimingBackend b(o); b.reserve(8192);
            const auto at = b.now_ps();
            auto r = b.read_batch({{at, {{0,16}, {8,16}, {4095,2}}}, {at, {{128,4}}}, {at, {}}});
            check(r[0].sectors == 3 && r[1].sectors == 1 && r[2].sectors == 0, "exact sector union and empty contribution");
            check(b.statistics().requests == 2 && b.statistics().sectors == 4, "batch logical counters");
            check(b.statistics().peak_resident_fragments <= o.resident_fragments && b.drained(), "Host resident budget drained");
            const auto before = b.statistics().requests;
            r = b.read_batch({{b.now_ps(), {{0,4}}}, {b.now_ps(), {{8192,4}}}});
            check(r[0].status == Status::InvalidRange && b.statistics().requests == before, "validate whole batch before advancing");
        } else if (name == "fault") {
            o.fail_program = true; TimingBackend b(o);
            rejects([&] { b.reserve(4096); });
            rejects([&] { b.read(0, 4); });
            check(!b.drained(), "failed initialization must poison session");
        } else if (name == "limit") {
            o.max_events_per_batch = 1; TimingBackend b(o);
            rejects([&] { b.reserve(4096); });
            rejects([&] { b.read(0, 4); });
        } else if (name == "invalid") {
            TimingBackend b;
            rejects([&] { b.reserve(1); });
            rejects([&] { b.reserve(std::numeric_limits<std::uint64_t>::max()); });
            check(b.read(0, 4).status == Status::InvalidRange, "uninitialized read");
            b.reserve(4096);
            check(b.read(4095, 2).status == Status::InvalidRange, "range overrun");
            check(b.read(UINT64_MAX, 4).status == Status::InvalidRange, "overflow range");
            check(b.read(0, 0).status == Status::InvalidRange, "empty read");
            check(b.read(0, 4097).status == Status::InvalidRange, "oversized read");
            check(b.statistics().requests == 0 && b.drained(), "rejections have no backend effect");
        } else if (name == "cold_hot") {
            TimingBackend b;
            check(b.reserve(4096) == 0 && b.reserve(4096) == 4096, "nonoverlapping ranges");
            const auto cold = b.read(0, 4);
            const auto hot = b.read(4, 4, 123000);
            check(cold.status == Status::Ok && hot.status == Status::Ok, "read success");
            check(hot.begin_ps == cold.end_ps + 123000, "GPU compute gap advances virtual time");
            check(cold.end_ps - cold.begin_ps > hot.end_ps - hot.begin_ps, "persistent bank cache affects timing");
            const auto stats = b.statistics();
            check(stats.device.programs == 2 && stats.device.senses == 1 && stats.device.cache_hits == 1,
                  "real media initialization and read/cache paths");
            check(b.drained(), "clean drain");
        } else if (name == "split" || name == "backpressure") {
            if (name == "backpressure") {
                o.outstanding = 1; o.timing.buffers.ingress = {1, 4096}; o.timing.buffers.egress = {1, 64};
            }
            TimingBackend b(o); b.reserve(8192);
            const auto cross = b.read(4095, 2);
            check(cross.status == Status::Ok && cross.sectors == 2, "cross-page 64B splitting");
            const auto full = b.read(1, 4096);
            check(full.status == Status::Ok && full.sectors == 65, "unaligned full interval covers 65 sectors");
            const auto s = b.statistics();
            check(s.sectors == 67 && s.peak_resident_fragments <= o.resident_fragments, "bounded Host resident fragments");
            check(s.registered_globals == s.completed_globals && s.host.logical_success == s.registered_fragments && s.device.accepted == s.device.delivered, "Host final completions and exactly-once responses");
            check(b.drained(), "buffers, subscriptions and events drained");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
