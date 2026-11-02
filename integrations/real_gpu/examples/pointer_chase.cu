#include "hbflab/runtime.hpp"
#include "hbflab/device.cuh"
#include <iostream>
#include <string>
#include <vector>

using namespace hbflab;
constexpr unsigned steps = 16;
struct Result {
    std::uint32_t checksum = 0, value = 0xdeadbeef, completed = 0;
    Status status = Status::ProtocolError;
    AccessTrace trace[steps]{};
};
__global__ void chase(DeviceView view, const std::uint32_t* data, Result* results, bool invalid) {
    auto& out = results[threadIdx.x];
    DeviceClient client(view);
    std::uint32_t index = 0;
    for (unsigned i = 0; i < steps; ++i) {
        const auto* ptr = invalid ? data + 2048 : data + index;
        out.status = client.load_u32(ptr, out.value, &out.trace[i]);
        if (out.status != Status::Ok) return;
        index = out.value;
        out.checksum += index;
        ++out.completed;
    }
}
void check(bool b, const char* why) { if (!b) throw std::runtime_error(why); }
void ck(cudaError_t e) { if (e != cudaSuccess) throw std::runtime_error(cudaGetErrorString(e)); }
int main(int argc, char** argv) {
    cudaStream_t stream = nullptr;
    Result* device_results = nullptr;
    try {
        const std::string mode = argc > 1 ? argv[1] : "normal";
        check(mode == "normal" || mode == "scaled" || mode == "invalid" || mode == "timeout" ||
              mode == "no_service" || mode == "backend_error" || mode == "unsupported", "unknown mode");
        RuntimeOptions options;
        if (mode == "scaled") options.time_scale = 100000;
        if (mode == "timeout") options.timeout_ns = 1;
        if (mode == "no_service") options.timeout_ns = 5000000;
        // Legal positive timing, but adding it to the initialized model clock
        // overflows. Exercise a real EventLab exception at read time, not a mock.
        if (mode == "backend_error") options.backend.timing.media_timing.sense = hbf_eventlab::Tick{UINT64_MAX};
        Runtime runtime(options);
        std::vector<std::uint32_t> source(2048);
        for (unsigned i = 0; i < source.size(); ++i) source[i] = (i * 17 + 1025) % 2048;
        const auto* data = runtime.upload(source.data(), source.size() * sizeof(source[0]));
        const auto view = runtime.view();
        ck(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        ck(cudaMalloc(reinterpret_cast<void**>(&device_results), 2 * sizeof(Result)));
        Result results[2]{};
        ck(cudaMemcpy(device_results, results, sizeof(results), cudaMemcpyHostToDevice));
        chase<<<1, mode == "unsupported" ? 2 : 1, 0, stream>>>(view, data, device_results, mode == "invalid");
        ck(cudaGetLastError());
        if (mode == "no_service") ck(cudaStreamSynchronize(stream));
        const auto served_status = runtime.serve(stream);
        ck(cudaMemcpy(results, device_results, sizeof(results), cudaMemcpyDeviceToHost));
        const auto& r = results[0];
        if (mode == "invalid" || mode == "unsupported") {
            check(r.status == (mode == "invalid" ? Status::InvalidRange : Status::Unsupported), "device must reject access");
            check(r.value == 0xdeadbeef && r.completed == 0, "rejected access must not execute the real load");
            check(runtime.statistics().requests == 0, "rejection has no backend read effect");
            if (mode == "invalid") check(served_status == Status::InvalidRange && !runtime.drained(), "invalid frontier terminates the session");
            else check(served_status == Status::Ok && runtime.drained(), "shape rejection has no slot effect");
            if (mode == "unsupported") check(results[1].status == Status::Unsupported, "all invalid producers rejected");
        } else if (mode == "timeout" || mode == "no_service" || mode == "backend_error") {
            const auto expected_status = mode == "backend_error" ? Status::BackendError : Status::Timeout;
            check(r.status == expected_status && served_status == expected_status, "terminal failure must reach both endpoints");
            check(r.value == 0xdeadbeef && r.completed == 0 && !runtime.drained(), "timeout must not permit data consumption");
            bool rejected = false;
            try { runtime.view(); } catch (const std::exception&) { rejected = true; }
            check(rejected, "timed-out session cannot reuse a stale completion");
            if (mode == "no_service") check(runtime.statistics().requests == 0, "no host service was executed");
        } else {
            std::uint32_t expected = 0, index = 0;
            for (unsigned i = 0; i < steps; ++i) { index = source[index]; expected += index; }
            check(served_status == Status::Ok && r.status == Status::Ok && r.completed == steps && r.checksum == expected,
                  "GPU dependent pointer chase differs from independent CPU reference");
            bool observed_gate = false;
            std::cout << "step,modeled_ps,delay_ns,host_roundtrip_ns,total_access_ns,overshoot_ns\n";
            for (unsigned i = 0; i < steps; ++i) {
                const auto& t = r.trace[i];
                check(t.status == Status::Ok && t.resumed_ns >= t.completion_seen_ns &&
                      t.resumed_ns - t.arrival_ns >= t.delay_ns, "load resumed before completion/deadline");
                observed_gate |= t.completion_seen_ns - t.arrival_ns < t.delay_ns;
                std::cout << i << ',' << t.modeled_ps << ',' << t.delay_ns << ','
                          << t.completion_seen_ns - t.arrival_ns << ',' << t.resumed_ns - t.arrival_ns << ','
                          << t.resumed_ns - t.arrival_ns - t.delay_ns << '\n';
            }
            if (mode == "scaled") check(observed_gate, "scaled experiment must observe actual GPU target waiting");
            const auto s = runtime.statistics();
            check(s.requests == steps && s.sectors == steps && s.device.senses > 0 && s.device.cache_hits > 0,
                  "GPU accesses did not pass through persistent EventLab");
            check(runtime.drained(), "successful run must drain");
            std::cout << "requests=" << s.requests << " senses=" << s.device.senses
                      << " cache_hits=" << s.device.cache_hits << " checksum=" << r.checksum << '\n';
            // Reuse the same mailbox with increasing tickets, preserving media/cache.
            Result again[2]{};
            ck(cudaMemcpy(device_results, again, sizeof(again), cudaMemcpyHostToDevice));
            chase<<<1, 1, 0, stream>>>(runtime.view(), data, device_results, false);
            ck(cudaGetLastError());
            check(runtime.serve(stream) == Status::Ok, "second launch failed");
            ck(cudaMemcpy(again, device_results, sizeof(again), cudaMemcpyDeviceToHost));
            check(again[0].checksum == expected && runtime.statistics().requests == 2 * steps && runtime.drained(), "mailbox reuse or state continuity failed");
        }
        ck(cudaFree(device_results)); device_results = nullptr;
        ck(cudaStreamDestroy(stream)); stream = nullptr;
        std::cout << "PASS " << mode << '\n'; return 0;
    } catch (const std::exception& e) {
        if (stream) cudaStreamSynchronize(stream);
        if (device_results) cudaFree(device_results);
        if (stream) cudaStreamDestroy(stream);
        std::cerr << e.what() << '\n'; return 1;
    }
}
