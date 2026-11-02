#pragma once
#include "hbflab/protocol.hpp"
#include <cuda/atomic>
#include <cuda_runtime.h>

namespace hbflab {
template<class T> __host__ __device__ inline T acquire(T& word) {
    return cuda::atomic_ref<T, cuda::thread_scope_system>(word).load(cuda::memory_order_acquire);
}
template<class T> __host__ __device__ inline void release(T& word, T value) {
    cuda::atomic_ref<T, cuda::thread_scope_system>(word).store(value, cuda::memory_order_release);
}
__device__ inline std::uint64_t gpu_now_ns() {
    std::uint64_t value;
    asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(value));
    return value;
}
// Collective over one CTA. Every physical thread calls the same helper each round.
// Thread slots are owned by threadIdx.x; warp slots by physical lane 0 of each warp.
class DeviceClient {
    DeviceView view_;
    __device__ bool shape(ProducerMode mode) const {
        return view_.version == abi_version && view_.mailbox && view_.ranges &&
            view_.range_count && view_.range_count <= max_ranges && view_.timeout_ns &&
            view_.threads && view_.threads <= max_threads && view_.mode == mode &&
            view_.slot_count == (mode == ProducerMode::Thread ? view_.threads : (view_.threads + 31) / 32) &&
            gridDim.x == 1 && gridDim.y == 1 && gridDim.z == 1 &&
            blockDim.x == view_.threads && blockDim.y == 1 && blockDim.z == 1;
    }
    __device__ Status span(const void* source, void* output, std::uint32_t bytes,
                           std::uint32_t alignment, Span& result) const {
        const auto ptr = reinterpret_cast<std::uint64_t>(source);
        if (!source || !output || !bytes || bytes > max_read_bytes || ptr % alignment)
            return Status::InvalidRange;
        for (unsigned i = 0; i < view_.range_count; ++i) {
            const auto r = view_.ranges[i];
            if (ptr >= r.gpu_base && r.bytes >= bytes && ptr - r.gpu_base <= r.bytes - bytes) {
                result = {ptr, bytes, i}; return Status::Ok;
            }
        }
        return Status::InvalidRange;
    }
    __device__ static void copy_bytes(const void* source, void* output, std::uint32_t bytes) {
        const auto* from = static_cast<const volatile unsigned char*>(source);
        auto* to = static_cast<unsigned char*>(output);
        for (unsigned i = 0; i < bytes; ++i) to[i] = from[i];
    }
    __device__ static Status finish(AccessTrace& t, Status status, AccessTrace* trace) {
        t.status = status; t.resumed_ns = gpu_now_ns();
        if (trace) *trace = t;
        return status;
    }
    __device__ Status exchange(Request& request, AccessTrace& t) {
        auto& slot = view_.mailbox[request.producer];
        if (const auto terminal = acquire(slot.terminal)) return static_cast<Status>(terminal);
        const auto last = acquire(slot.req_sequence);
        if (last == ~std::uint64_t{0} || acquire(slot.done_sequence) != last) {
            release(slot.terminal, static_cast<std::uint32_t>(Status::ProtocolError));
            return Status::ProtocolError;
        }
        t.arrival_ns = gpu_now_ns();
        request.ticket = last + 1; request.arrival_ns = t.arrival_ns;
        slot.request = request;
        release(slot.req_sequence, request.ticket);
        while (acquire(slot.done_sequence) != request.ticket) {
            if (const auto terminal = acquire(slot.terminal)) return static_cast<Status>(terminal);
            if (gpu_now_ns() - t.arrival_ns >= view_.timeout_ns) {
                release(slot.terminal, static_cast<std::uint32_t>(Status::Timeout));
                return Status::Timeout;
            }
            __nanosleep(128);
        }
        t.completion_seen_ns = gpu_now_ns();
        const auto c = slot.completion;
        if (c.version != abi_version || c.ticket != request.ticket ||
            static_cast<unsigned>(c.status) > static_cast<unsigned>(Status::ProtocolError)) {
            release(slot.terminal, static_cast<std::uint32_t>(Status::ProtocolError));
            return Status::ProtocolError;
        }
        t.modeled_ps = c.modeled_ps; t.delay_ns = c.delay_ns;
        if (c.status != Status::Ok) {
            release(slot.terminal, static_cast<std::uint32_t>(c.status)); return c.status;
        }
        for (;;) {
            if (const auto terminal = acquire(slot.terminal)) return static_cast<Status>(terminal);
            const auto elapsed = gpu_now_ns() - t.arrival_ns;
            if (elapsed >= view_.timeout_ns) {
                release(slot.terminal, static_cast<std::uint32_t>(Status::Timeout)); return Status::Timeout;
            }
            if (elapsed >= c.delay_ns) break;
            __nanosleep(32);
        }
        return Status::Ok;
    }
    __device__ Status thread_access(const void* source, void* output, unsigned bytes,
                                   unsigned alignment, bool empty, AccessTrace* trace, bool copy = true) {
        AccessTrace t{};
        if (!shape(ProducerMode::Thread)) return finish(t, Status::Unsupported, trace);
        Request r{}; r.producer = threadIdx.x;
        if (!empty) { r.span_count = 1; r.status = span(source, output, bytes, alignment, r.spans[0]); }
        const auto status = exchange(r, t);
        if (status == Status::Ok && !empty && copy) copy_bytes(source, output, bytes);
        finish(t, status, trace);
        // No next-round request can precede the largest prior target completion.
        __syncthreads();
        return status;
    }
    __device__ static std::uint64_t broadcast(unsigned physical, std::uint64_t value) {
        return __shfl_sync(physical, static_cast<unsigned long long>(value), 0);
    }
    __device__ Status warp_access(const void* source, void* output, unsigned bytes,
                                 unsigned alignment, unsigned mask, AccessTrace* trace, bool copy = true) {
        AccessTrace t{};
        if (!shape(ProducerMode::Warp)) return finish(t, Status::Unsupported, trace);
        const unsigned lane = threadIdx.x % 32, warp = threadIdx.x / 32;
        const unsigned count = (view_.threads - warp * 32 < 32) ? view_.threads - warp * 32 : 32;
        const unsigned physical = count == 32 ? 0xffffffffu : (1u << count) - 1;
        const unsigned selected = mask & physical;
        const bool participant = (selected & (1u << lane)) != 0;
        Span own{};
        Status local = participant ? span(source, output, bytes, alignment, own) : Status::Ok;
        const unsigned leader_mask = __shfl_sync(physical, mask, 0);
        if (__ballot_sync(physical, leader_mask != mask)) local = Status::ProtocolError;
        Request r{}; r.producer = warp;
        // All physical lanes execute every shuffle. Only lane 0 keeps the list.
        for (unsigned l = 0; l < count; ++l) {
            const auto address = __shfl_sync(physical, static_cast<unsigned long long>(own.address), l);
            const auto length = __shfl_sync(physical, own.bytes, l);
            const auto range = __shfl_sync(physical, own.range, l);
            const auto error = static_cast<Status>(__shfl_sync(physical, static_cast<unsigned>(local), l));
            if (lane == 0) {
                if (error != Status::Ok) r.status = error;
                if ((selected & (1u << l)) && error == Status::Ok) r.spans[r.span_count++] = {address, length, range};
            }
        }
        Status status = Status::Ok;
        if (lane == 0) {
            // Exact interval union within each range; never charge sparse holes.
            for (unsigned i = 1; i < r.span_count; ++i) {
                const auto value = r.spans[i]; unsigned j = i;
                while (j && (r.spans[j-1].range > value.range ||
                       (r.spans[j-1].range == value.range && r.spans[j-1].address > value.address))) {
                    r.spans[j] = r.spans[j-1]; --j;
                }
                r.spans[j] = value;
            }
            unsigned merged = 0, total = 0;
            for (unsigned i = 0; i < r.span_count; ++i) {
                const auto v = r.spans[i];
                if (merged && r.spans[merged-1].range == v.range &&
                    v.address <= r.spans[merged-1].address + r.spans[merged-1].bytes) {
                    auto& previous = r.spans[merged-1];
                    const auto end = v.address + v.bytes;
                    if (end > previous.address + previous.bytes)
                        previous.bytes = static_cast<unsigned>(end - previous.address);
                } else r.spans[merged++] = v;
            }
            r.span_count = merged;
            for (unsigned i = 0; i < merged; ++i) total += r.spans[i].bytes;
            if (total > max_read_bytes) r.status = Status::InvalidRange;
            status = exchange(r, t);
        }
        status = static_cast<Status>(__shfl_sync(physical, static_cast<unsigned>(status), 0));
        t.arrival_ns = broadcast(physical, t.arrival_ns);
        t.completion_seen_ns = broadcast(physical, t.completion_seen_ns);
        t.modeled_ps = broadcast(physical, t.modeled_ps);
        t.delay_ns = broadcast(physical, t.delay_ns);
        if (status == Status::Ok && participant && copy) copy_bytes(source, output, bytes);
        finish(t, status, trace);
        __syncthreads();
        return status;
    }
public:
    __device__ explicit DeviceClient(DeviceView view) : view_(view) {}
    // PTX frontend: model and wait, then let the original typed instruction load.
    // Every physical lane calls this, including lanes with a false predicate.
    __device__ Status gate_load(const void* source, unsigned bytes, bool active) {
        unsigned char dummy;
        if (view_.mode == ProducerMode::Thread)
            return thread_access(source, &dummy, bytes, bytes, !active, nullptr, false);
        const unsigned count = min(32u, view_.threads - (threadIdx.x / 32) * 32);
        const unsigned physical = count == 32 ? 0xffffffffu : (1u << count) - 1;
        const unsigned mask = __ballot_sync(physical, active);
        return warp_access(source, &dummy, bytes, bytes, mask, nullptr, false);
    }
    __device__ Status read_span(const void* source, void* output, std::uint32_t bytes,
                               AccessTrace* trace = nullptr, bool active = true) {
        return thread_access(source, output, bytes, 1, !active, trace);
    }
    __device__ Status skip(AccessTrace* trace = nullptr) {
        return thread_access(nullptr, nullptr, 0, 1, true, trace);
    }
    template<class T> __device__ Status load(const T* source, T& output,
                                           AccessTrace* trace = nullptr, bool active = true) {
        static_assert(std::is_trivially_copyable<T>::value &&
            (sizeof(T)==1 || sizeof(T)==2 || sizeof(T)==4 || sizeof(T)==8 || sizeof(T)==16),
            "typed loads require a trivially-copyable 1/2/4/8/16-byte type");
        return thread_access(source, &output, sizeof(T), alignof(T), !active, trace);
    }
    __device__ Status load_u32(const std::uint32_t* source, std::uint32_t& output, AccessTrace* trace = nullptr) {
        return load(source, output, trace);
    }
    __device__ Status warp_read_span(const void* source, void* output, std::uint32_t bytes,
                                    unsigned mask = 0xffffffffu, AccessTrace* trace = nullptr) {
        return warp_access(source, output, bytes, 1, mask, trace);
    }
    template<class T> __device__ Status warp_load(const T* source, T& output,
                                                unsigned mask = 0xffffffffu, AccessTrace* trace = nullptr) {
        static_assert(std::is_trivially_copyable<T>::value &&
            (sizeof(T)==1 || sizeof(T)==2 || sizeof(T)==4 || sizeof(T)==8 || sizeof(T)==16),
            "typed loads require a trivially-copyable 1/2/4/8/16-byte type");
        return warp_access(source, &output, sizeof(T), alignof(T), mask, trace);
    }
};
}
