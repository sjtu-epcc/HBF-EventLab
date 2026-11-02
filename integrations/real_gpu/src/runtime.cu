#include "hbflab/runtime.hpp"
#include "hbflab/device.cuh"
#include "hbflab/frontier.hpp"
#include <cuda.h>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace hbflab {
namespace {
void cuda_check(cudaError_t code) {
    if (code != cudaSuccess) throw std::runtime_error(std::string("CUDA: ") + cudaGetErrorString(code));
}
}
struct Runtime::Impl {
    RuntimeOptions options;
    TimingBackend backend;
    Mailbox* host = nullptr;
    Mailbox* device = nullptr;
    Range* device_ranges = nullptr;
    std::vector<Range> ranges;
    std::vector<void*> allocations;
    std::vector<ServiceTrace> audit;
    bool frozen = false, poisoned = false;
    int ordinal = 0;
    std::uint32_t slots = 0;
    std::uint64_t served = 0;
    const std::thread::id owner = std::this_thread::get_id();
    explicit Impl(RuntimeOptions o) : options(o), backend(o.backend) {
        if (!o.time_scale || o.time_scale > 1000000 || !o.timeout_ns || o.timeout_ns > 1000000000 ||
            !o.threads || o.threads > max_threads ||
            (o.mode != ProducerMode::Thread && o.mode != ProducerMode::Warp))
            throw std::invalid_argument("invalid scale, timeout or producer layout");
        slots = o.mode == ProducerMode::Thread ? o.threads : (o.threads + 31) / 32;
        cuda_check(cudaGetDevice(&ordinal));
        cudaDeviceProp prop{};
        cuda_check(cudaGetDeviceProperties(&prop, ordinal));
        if (!prop.canMapHostMemory || !prop.unifiedAddressing || prop.major < 7 || o.threads > prop.maxThreadsPerBlock)
            throw std::runtime_error("bridge requires mapped host memory, UVA, SM70+ and a resident CTA");
        try {
            cuda_check(cudaHostAlloc(reinterpret_cast<void**>(&host), slots * sizeof(Mailbox), cudaHostAllocMapped));
            for (unsigned i = 0; i < slots; ++i) new (host + i) Mailbox{};
            cuda_check(cudaHostGetDevicePointer(reinterpret_cast<void**>(&device), host, 0));
            cuda_check(cudaMalloc(reinterpret_cast<void**>(&device_ranges), max_ranges * sizeof(Range)));
        } catch (...) {
            if (host) cudaFreeHost(host);
            throw;
        }
    }
    ~Impl() {
        int previous = ordinal;
        cudaGetDevice(&previous); cudaSetDevice(ordinal); cudaDeviceSynchronize();
        for (auto p : allocations) cudaFree(p);
        if (device_ranges) cudaFree(device_ranges);
        if (host) cudaFreeHost(host);
        cudaSetDevice(previous);
    }
    void current() const {
        if (owner != std::this_thread::get_id()) throw std::runtime_error("runtime used on a different host thread");
        int selected = -1; cuda_check(cudaGetDevice(&selected));
        if (selected != ordinal) throw std::runtime_error("runtime used on a different CUDA device");
    }
    void fail(Status status) {
        poisoned = true;
        for (unsigned i = 0; i < slots; ++i) release(host[i].terminal, static_cast<std::uint32_t>(status));
    }
    Status process(const std::vector<Request>& requests, std::uint64_t gpu_epoch, std::uint64_t model_epoch) {
        audit.clear();
        std::vector<FrontierItem> ordered;
        try { ordered = order_frontier(requests, served + 1, gpu_epoch, model_epoch, backend.now_ps(), options.time_scale); }
        catch (...) { return Status::ProtocolError; }
        // Validate the ENTIRE frontier before any timing state or real data changes.
        std::vector<TimedRead> work;
        for (const auto& item : ordered) {
            const auto& r = requests[item.producer];
            if (r.status != Status::Ok) {
                return static_cast<unsigned>(r.status) <= static_cast<unsigned>(Status::ProtocolError)
                    ? r.status : Status::ProtocolError;
            }
            if (r.span_count > max_spans) return Status::ProtocolError;
            TimedRead parent{item.arrival_ps, {}};
            std::uint64_t total = 0;
            for (unsigned j = 0; j < r.span_count; ++j) {
                const auto& span = r.spans[j];
                if (span.range >= ranges.size()) return Status::InvalidRange;
                const auto& range = ranges[span.range];
                if (!span.bytes || span.bytes > max_read_bytes || span.address < range.gpu_base ||
                    range.bytes < span.bytes || span.address - range.gpu_base > range.bytes - span.bytes)
                    return Status::InvalidRange;
                total += span.bytes;
                parent.spans.push_back({range.logical_base + span.address - range.gpu_base, span.bytes});
            }
            if (total > max_read_bytes) return Status::InvalidRange;
            work.push_back(std::move(parent));
        }
        std::vector<Completion> completions(slots);
        try {
            const auto results = backend.read_batch(work);
            for (std::size_t i = 0; i < ordered.size(); ++i) {
                const auto& item = ordered[i];
                const auto& result = results[i];
                if (result.status != Status::Ok) return result.status;
                auto& c = completions[item.producer];
                c.ticket = item.ticket; c.status = result.status;
                c.modeled_ps = result.end_ps - result.begin_ps;
                c.delay_ns = scale_ticks(c.modeled_ps, options.time_scale, 1000, true);
                if (c.delay_ns >= options.timeout_ns) return Status::Timeout;
                audit.push_back({item.producer, requests[item.producer].span_count, result.sectors,
                                 item.ticket, item.arrival_ns, item.arrival_ps, result.end_ps});
            }
        } catch (...) { return Status::BackendError; }
        // All completions prepared before publication; no request is reported ready
        // if some member failed during frontier validation or backend execution.
        for (unsigned i = 0; i < slots; ++i) {
            host[i].completion = completions[i];
            release(host[i].done_sequence, served + 1);
        }
        ++served;
        return Status::Ok;
    }
};
Runtime::Runtime(RuntimeOptions o) : impl_(std::make_unique<Impl>(o)) {}
Runtime::~Runtime() = default;
const std::uint32_t* Runtime::upload(const void* source, std::size_t bytes) {
    auto& s = *impl_; s.current();
    if (s.frozen || s.poisoned || !source || !bytes || s.ranges.size() == max_ranges)
        throw std::invalid_argument("upload requires unfrozen runtime, nonempty bytes and a free range slot");
    const auto padded = hbf_eventlab::checked_mul(hbf_eventlab::checked_add(bytes, 4095) / 4096, 4096);
    void* allocation = nullptr;
    cuda_check(cudaMalloc(&allocation, bytes));
    try {
        cuda_check(cudaMemcpy(allocation, source, bytes, cudaMemcpyHostToDevice));
        const auto logical = s.backend.reserve(padded);
        s.allocations.push_back(allocation);
        try { s.ranges.push_back({reinterpret_cast<std::uint64_t>(allocation), bytes, logical}); }
        catch (...) { s.allocations.pop_back(); throw; }
    } catch (...) {
        cudaFree(allocation); s.poisoned = true; throw;
    }
    return static_cast<const std::uint32_t*>(allocation);
}
void Runtime::register_readonly(const void* source, std::size_t bytes) {
    auto& s=*impl_; s.current();
    if (s.frozen || s.poisoned || !source || !bytes || s.ranges.size()==max_ranges)
        throw std::invalid_argument("borrow requires unfrozen Runtime, nonempty range and free slot");
    const auto address=reinterpret_cast<CUdeviceptr>(source);
    CUdeviceptr base=0; std::size_t allocation_bytes=0;
    CUcontext current=nullptr, allocation_context=nullptr;
    if (cuCtxGetCurrent(&current)!=CUDA_SUCCESS ||
        cuPointerGetAttribute(&allocation_context,CU_POINTER_ATTRIBUTE_CONTEXT,address)!=CUDA_SUCCESS ||
        current!=allocation_context || cuMemGetAddressRange(&base,&allocation_bytes,address)!=CUDA_SUCCESS ||
        address<base || bytes>allocation_bytes || address-base>allocation_bytes-bytes)
        throw std::invalid_argument("borrowed range must belong to a device allocation in this context");
    for (const auto& r:s.ranges) {
        const bool overlap=address>=r.gpu_base?address-r.gpu_base<r.bytes:r.gpu_base-address<bytes;
        if (overlap) throw std::invalid_argument("borrowed range overlaps a registered interval");
    }
    const auto padded=hbf_eventlab::checked_mul(hbf_eventlab::checked_add(bytes,4095)/4096,4096);
    try { const auto logical=s.backend.reserve(padded); s.ranges.push_back({address,bytes,logical}); }
    catch (...) { s.poisoned=true; throw; }
}
DeviceView Runtime::view() {
    auto& s = *impl_; s.current();
    if (s.poisoned || s.ranges.empty()) throw std::runtime_error("runtime has no valid ranges");
    if (!s.frozen) {
        cuda_check(cudaMemcpy(s.device_ranges, s.ranges.data(), s.ranges.size() * sizeof(Range), cudaMemcpyHostToDevice));
        s.frozen = true;
    }
    return {s.device, s.device_ranges, s.options.timeout_ns, static_cast<std::uint32_t>(s.ranges.size()),
            abi_version, s.options.threads, s.slots, s.options.mode};
}
Status Runtime::serve(cudaStream_t stream) {
    auto& s = *impl_; s.current();
    if (!s.frozen || s.poisoned) throw std::runtime_error("serve requires a valid frozen runtime");
    cudaEvent_t finished = nullptr;
    cuda_check(cudaEventCreateWithFlags(&finished, cudaEventDisableTiming));
    Status status = Status::Ok;
    bool have_epoch = false;
    std::uint64_t gpu_epoch = 0, model_epoch = 0;
    try {
        cuda_check(cudaEventRecord(finished, stream));
        for (;;) {
            bool all = true;
            if (status == Status::Ok) {
                for (unsigned i = 0; i < s.slots; ++i) {
                    const auto terminal = acquire(s.host[i].terminal);
                    if (terminal) { status = static_cast<Status>(terminal); break; }
                    const auto seq = acquire(s.host[i].req_sequence);
                    if (seq == s.served) all = false;
                    else if (s.served == UINT64_MAX || seq != s.served + 1) { status = Status::ProtocolError; break; }
                }
                if (status == Status::Ok && all) {
                    std::vector<Request> requests;
                    for (unsigned i = 0; i < s.slots; ++i) requests.push_back(s.host[i].request);
                    if (!have_epoch) {
                        gpu_epoch = requests.front().arrival_ns;
                        for (const auto& r : requests) gpu_epoch = std::min(gpu_epoch, r.arrival_ns);
                        model_epoch = s.backend.now_ps(); have_epoch = true;
                    }
                    status = s.process(requests, gpu_epoch, model_epoch);
                }
                if (status != Status::Ok) s.fail(status);
            }
            const auto query = cudaEventQuery(finished);
            if (query == cudaSuccess) {
                // Re-read after stream completion: the last iteration may have
                // observed no request immediately before the GPU published one.
                for (unsigned i = 0; i < s.slots; ++i) {
                    if (const auto terminal = acquire(s.host[i].terminal)) status = static_cast<Status>(terminal);
                    if (status == Status::Ok && acquire(s.host[i].req_sequence) != s.served)
                        status = Status::ProtocolError;
                }
                if (status != Status::Ok) s.fail(status);
                break;
            }
            if (query != cudaErrorNotReady) cuda_check(query);
            std::this_thread::yield();
        }
        cuda_check(cudaEventDestroy(finished));
        return status;
    } catch (...) {
        s.fail(Status::BackendError);
        cudaEventDestroy(finished);
        throw;
    }
}
BackendStats Runtime::statistics() const { return impl_->backend.statistics(); }
std::vector<ServiceTrace> Runtime::last_frontier() const { return impl_->audit; }
bool Runtime::drained() const {
    if (impl_->poisoned || !impl_->backend.drained()) return false;
    for (unsigned i = 0; i < impl_->slots; ++i)
        if (acquire(impl_->host[i].req_sequence) != acquire(impl_->host[i].done_sequence)) return false;
    return true;
}
}
