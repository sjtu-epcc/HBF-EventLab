#include "hbflab/runtime.hpp"
#include "hbflab/device.cuh"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace hbflab;
enum Mode { Typed, SpanRead, Threads, Skip, Dense, Sparse, Overlap, Partial, Masked,
            Empty, Ranges, Invalid, Alignment, Oversize, BadMask, Timeout, Pressure, MaxThreads,
            WarpOverflow, BackendError, Scaled, WarpTyped, MaxWarps };
struct alignas(16) Vector16 { std::uint32_t x, y, z, w; };
struct Values { std::uint8_t a; std::uint16_t b; std::uint32_t c; std::uint64_t d; Vector16 e; };
struct Result {
    Values typed;
    unsigned char data[4096];
    AccessTrace traces[5];
    Status status;
};
__host__ __device__ bool warp_mode(Mode mode) {
    return mode == Dense || mode == Sparse || mode == Overlap || mode == Partial ||
        mode == Masked || mode == Empty || mode == Ranges || mode == BadMask || mode == WarpOverflow ||
        mode == WarpTyped || mode == MaxWarps;
}
__host__ __device__ unsigned offset_for(Mode mode, unsigned tid) {
    if (mode == SpanRead) return 4095 + tid * 8192;
    if (mode == Pressure) return 1 + tid * 4096;
    if (warp_mode(mode)) {
        const unsigned lane = tid % 32, base = (tid / 32) * 8192;
        if (mode == Sparse) return base + lane * 128;
        if (mode == Overlap) return base + (lane % 4) * 4;
        if (mode == WarpOverflow) return base + lane * 256;
        return base + lane * 4;
    }
    return tid * 128 + 63;
}
__host__ __device__ unsigned length_for(Mode mode) {
    if (mode == SpanRead || mode == Pressure) return 4096;
    if (mode == Oversize) return 4097;
    if (mode == WarpOverflow) return 256;
    if (mode == Threads || mode == Skip) return 97;
    return 4;
}
__host__ __device__ bool participating(Mode mode, unsigned tid) {
    return mode != Empty && !(mode == Masked && tid % 2 == 0) && !(mode == Skip && tid % 3 == 0);
}
__global__ void access(DeviceView view, const unsigned char* source, const unsigned char* second,
                       Result* output, Mode mode) {
    const unsigned tid = threadIdx.x;
    auto& out = output[tid];
    DeviceClient client(view);
    if (mode == Typed || mode == WarpTyped) {
        const auto* base = source + tid * 128;
        out.status = mode == Typed ? client.load(base, out.typed.a, &out.traces[0]) : client.warp_load(base, out.typed.a, 0xffffffffu, &out.traces[0]);
        if (out.status != Status::Ok) return;
        out.status = mode == Typed ? client.load(reinterpret_cast<const std::uint16_t*>(base+2), out.typed.b, &out.traces[1]) : client.warp_load(reinterpret_cast<const std::uint16_t*>(base+2), out.typed.b, 0xffffffffu, &out.traces[1]);
        if (out.status != Status::Ok) return;
        out.status = mode == Typed ? client.load(reinterpret_cast<const std::uint32_t*>(base+4), out.typed.c, &out.traces[2]) : client.warp_load(reinterpret_cast<const std::uint32_t*>(base+4), out.typed.c, 0xffffffffu, &out.traces[2]);
        if (out.status != Status::Ok) return;
        out.status = mode == Typed ? client.load(reinterpret_cast<const std::uint64_t*>(base+8), out.typed.d, &out.traces[3]) : client.warp_load(reinterpret_cast<const std::uint64_t*>(base+8), out.typed.d, 0xffffffffu, &out.traces[3]);
        if (out.status != Status::Ok) return;
        out.status = mode == Typed ? client.load(reinterpret_cast<const Vector16*>(base+16), out.typed.e, &out.traces[4]) : client.warp_load(reinterpret_cast<const Vector16*>(base+16), out.typed.e, 0xffffffffu, &out.traces[4]);
        return;
    }
    for (unsigned round = 0; round < 3; ++round) {
        auto offset = offset_for(mode, tid);
        if (mode == Invalid && tid+1 == view.threads) offset = 65536;
        const auto* base = mode == Ranges && (tid % 2) ? second : source;
        if (warp_mode(mode)) {
            unsigned mask = mode == Empty ? 0 : mode == Masked ? 0xaaaaaaaau : 0xffffffffu;
            if (mode == BadMask && tid % 2) mask = 0x55555555u;
            if (mode == Dense || mode == Partial)
                out.status = client.warp_load(reinterpret_cast<const std::uint32_t*>(base+offset), out.typed.c, mask, &out.traces[round]);
            else out.status = client.warp_read_span(base+offset, out.data, length_for(mode), mask, &out.traces[round]);
        } else if (mode == Alignment) {
            out.status = client.load(reinterpret_cast<const std::uint64_t*>(source+1), out.typed.d, &out.traces[round]);
        } else {
            out.status = client.read_span(base+offset, out.data, length_for(mode), &out.traces[round], participating(mode, tid));
        }
        if (out.status != Status::Ok) return;
    }
}
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void ck(cudaError_t value) { if (value != cudaSuccess) throw std::runtime_error(cudaGetErrorString(value)); }
bool equal(const void* a, const void* b, std::size_t n) { return std::memcmp(a,b,n) == 0; }
int main(int argc, char** argv) {
    cudaStream_t stream = nullptr;
    Result* gpu_results = nullptr;
    try {
        const std::string name = argc > 1 ? argv[1] : "threads";
        const std::map<std::string, Mode> modes{
            {"typed",Typed},{"span",SpanRead},{"threads",Threads},{"skip",Skip},{"dense",Dense},
            {"sparse",Sparse},{"overlap",Overlap},{"partial",Partial},{"masked",Masked},{"empty",Empty},
            {"ranges",Ranges},{"invalid",Invalid},{"alignment",Alignment},{"oversize",Oversize},
            {"badmask",BadMask},{"timeout",Timeout},{"pressure",Pressure},{"max_threads",MaxThreads},
            {"warp_overflow",WarpOverflow},{"backend_error",BackendError},{"scaled",Scaled},
            {"warp_typed",WarpTyped},{"max_warps",MaxWarps}};
        const auto mode = modes.at(name);
        RuntimeOptions options;
        options.threads = mode == SpanRead ? 4 : mode == Typed || mode == Pressure ? 8 :
                          mode == Partial ? 45 : mode == MaxThreads || mode == MaxWarps ? 256 : 64;
        options.mode = warp_mode(mode) ? ProducerMode::Warp : ProducerMode::Thread;
        // This is a 256-slot correctness/stress fixture, not a 0.5s service SLA.
        // Allow the supported 1s watchdog on slower host/display-GPU systems;
        // keep the production default and deliberate Timeout scenario unchanged.
        if (mode == MaxThreads) options.timeout_ns = 1000000000;
        if (mode == Timeout) options.timeout_ns = 5000000;
        if (mode == Scaled) options.time_scale = 100000;
        if (mode == BackendError) options.backend.timing.media_timing.sense = hbf_eventlab::Tick{UINT64_MAX};
        if (mode == Pressure) {
            options.backend.outstanding = 1;
            options.backend.resident_fragments = 64;
            options.backend.timing.buffers.ingress = {1,4096}; options.backend.timing.buffers.egress = {1,64};
        }
        Runtime runtime(options);
        std::vector<unsigned char> source(65536), second(65536);
        for (std::size_t i = 0; i < source.size(); ++i) { source[i] = static_cast<unsigned char>(i*7+3); second[i] = static_cast<unsigned char>(i*13+7); }
        const auto* data = reinterpret_cast<const unsigned char*>(runtime.upload(source.data(), source.size()));
        const auto* other = mode == Ranges ? reinterpret_cast<const unsigned char*>(runtime.upload(second.data(), second.size())) : data;
        const auto view = runtime.view();
        std::vector<Result> output(options.threads);
        std::memset(output.data(), 0xa5, output.size()*sizeof(Result));
        const auto before = output;
        ck(cudaMalloc(reinterpret_cast<void**>(&gpu_results), output.size()*sizeof(Result)));
        ck(cudaMemcpy(gpu_results, output.data(), output.size()*sizeof(Result), cudaMemcpyHostToDevice));
        ck(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
        access<<<1,options.threads,0,stream>>>(view,data,other,gpu_results,mode);
        ck(cudaGetLastError());
        if (mode == Timeout) ck(cudaStreamSynchronize(stream)); // deliberately no service
        const auto status = runtime.serve(stream);
        ck(cudaMemcpy(output.data(),gpu_results,output.size()*sizeof(Result),cudaMemcpyDeviceToHost));
        const bool failure = mode == Invalid || mode == Alignment || mode == Oversize || mode == BadMask ||
                             mode == Timeout || mode == WarpOverflow || mode == BackendError;
        if (failure) {
            const auto expected = mode == BadMask ? Status::ProtocolError : mode == Timeout ? Status::Timeout :
                                  mode == BackendError ? Status::BackendError : Status::InvalidRange;
            check(status == expected && !runtime.drained(), "host must terminate the failed frontier");
            for (unsigned i = 0; i < output.size(); ++i) {
                check(output[i].status == expected, "failure must reach every producer/lane");
                check(equal(output[i].data,before[i].data,sizeof(Result::data)) &&
                      equal(&output[i].typed,&before[i].typed,sizeof(Values)), "failed access consumed real bytes");
            }
            if(mode != BackendError)check(runtime.statistics().requests == 0, "invalid/unserviced frontier must not enter backend");
            else check(runtime.last_frontier().empty(), "failed backend frontier must publish no successful access audit");
        } else {
            if(status != Status::Ok)std::cerr<<"collective status="<<static_cast<unsigned>(status)<<'\n';
            check(status == Status::Ok && runtime.drained(), "successful collective did not drain");
            bool waited = false;
            for (unsigned tid = 0; tid < output.size(); ++tid) {
                const auto& r = output[tid];
                check(r.status == Status::Ok,"lane status");
                if (mode == Typed || mode == WarpTyped) {
                    const auto* base = source.data()+tid*128;
                    check(equal(&r.typed.a,base,1) && equal(&r.typed.b,base+2,2) && equal(&r.typed.c,base+4,4) &&
                          equal(&r.typed.d,base+8,8) && equal(&r.typed.e,base+16,16),"typed load bit pattern");
                } else if (participating(mode,tid)) {
                    const auto* base = mode == Ranges && tid%2 ? second.data() : source.data();
                    const auto* result = mode == Dense || mode == Partial ? reinterpret_cast<const unsigned char*>(&r.typed.c) : r.data;
                    check(equal(result,base+offset_for(mode,tid),length_for(mode)),"span/warp output differs from CPU bytes");
                } else check(equal(r.data,before[tid].data,sizeof(Result::data)),"inactive lane changed output");
                for (unsigned n = 0; n < (mode == Typed || mode == WarpTyped ? 5u : 3u); ++n) {
                    const auto& t = r.traces[n];
                    check(t.status == Status::Ok && t.resumed_ns >= t.completion_seen_ns &&
                          t.resumed_ns-t.arrival_ns >= t.delay_ns,"lane resumed before readiness");
                    waited |= t.completion_seen_ns-t.arrival_ns < t.delay_ns;
                }
            }
            const auto stats = runtime.statistics();
            if (mode == Typed || mode == WarpTyped) check(stats.requests == view.slot_count*5 && stats.sectors == options.threads*5,"typed coverage");
            else {
                std::map<unsigned,std::set<std::uint64_t>> expected_sectors;
                for (unsigned tid = 0; tid < options.threads; ++tid) {
                    if (!participating(mode,tid)) continue;
                    const auto producer = warp_mode(mode) ? tid/32 : tid;
                    const auto start = offset_for(mode,tid) + (mode == Ranges && tid%2 ? 65536u : 0u);
                    for (auto sector = start/64; sector <= (start+length_for(mode)-1)/64; ++sector)
                        expected_sectors[producer].insert(sector);
                }
                std::uint64_t sectors = 0;
                for (const auto& p : expected_sectors) sectors += p.second.size();
                check(stats.requests == 3*expected_sectors.size() && stats.sectors == 3*sectors,"exact union accounting or empty contribution failed");
            }
            const auto audit = runtime.last_frontier();
            check(audit.size() == view.slot_count,"missing frontier member");
            for (unsigned i = 0; i < audit.size(); ++i) {
                const auto& a = audit[i];
                check(a.ticket == (mode == Typed || mode == WarpTyped ? 5u : 3u) && a.end_ps >= a.arrival_ps,"ticket/arrival lifecycle");
                if (i) check(std::tie(audit[i-1].arrival_ns,audit[i-1].producer) <= std::tie(a.arrival_ns,a.producer),"frontier sorted by polling order");
                if (mode == Dense || mode == Partial || mode == Overlap) check(a.spans == 1,"contiguous/duplicate lanes were not merged");
                if (mode == Sparse) check(a.spans == 32,"sparse holes must not be covered");
                if (mode == Masked) check(a.spans == 16,"masked gaps must remain sparse");
            }
            if (mode == Pressure) check(stats.backpressure > 0 && stats.peak_registered_globals <= options.backend.resident_fragments,"global pressure window");
            if (mode == Scaled) check(waited,"concurrent test never observed a GPU target wait");
            std::cout << "threads=" << options.threads << " slots=" << view.slot_count << " requests=" << stats.requests
                      << " sectors=" << stats.sectors << " peak_registered_globals=" << stats.peak_registered_globals << '\n';
        }
        ck(cudaFree(gpu_results)); gpu_results=nullptr;
        ck(cudaStreamDestroy(stream)); stream=nullptr;
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception& e) {
        if (stream) cudaStreamSynchronize(stream);
        if (gpu_results) cudaFree(gpu_results);
        if (stream) cudaStreamDestroy(stream);
        std::cerr << e.what() << '\n'; return 1;
    }
}
