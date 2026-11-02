#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hbflab {
constexpr std::uint32_t abi_version = 2;
constexpr std::uint32_t max_ranges = 16, max_threads = 256, max_spans = 32, max_read_bytes = 4096;
enum class Status : std::uint32_t {
    Ok = 0, InvalidRange = 1, Unsupported = 2, BackendError = 3,
    Timeout = 4, ProtocolError = 5
};
struct Range {
    std::uint64_t gpu_base = 0, bytes = 0, logical_base = 0;
};
enum class ProducerMode : std::uint32_t { Thread = 0, Warp = 1 };
struct Span {
    std::uint64_t address = 0;
    std::uint32_t bytes = 0, range = 0;
};
struct Request {
    std::uint64_t ticket = 0, arrival_ns = 0;
    std::uint32_t producer = 0, span_count = 0;
    Status status = Status::Ok;
    std::uint32_t version = abi_version;
    Span spans[max_spans]{};
};
struct Completion {
    std::uint64_t ticket = 0, modeled_ps = 0, delay_ns = 0;
    Status status = Status::ProtocolError;
    std::uint32_t version = abi_version;
};
// SPSC: only aligned system-scope load/store crosses PCIe. No mapped-memory RMW.
// req_sequence release publishes request; done_sequence release publishes completion.
// The next request acknowledges consumption of the previous completion.
struct alignas(64) Mailbox {
    alignas(64) std::uint64_t req_sequence = 0;
    Request request{};
    alignas(64) std::uint64_t done_sequence = 0;
    Completion completion{};
    alignas(64) std::uint32_t terminal = 0;
};
struct DeviceView {
    Mailbox* mailbox = nullptr;
    const Range* ranges = nullptr;
    std::uint64_t timeout_ns = 500000000;
    std::uint32_t range_count = 0, version = abi_version;
    std::uint32_t threads = 1, slot_count = 1;
    ProducerMode mode = ProducerMode::Thread;
};
struct AccessTrace {
    std::uint64_t arrival_ns = 0, completion_seen_ns = 0, resumed_ns = 0;
    std::uint64_t modeled_ps = 0, delay_ns = 0;
    Status status = Status::ProtocolError;
};
static_assert(std::is_trivially_copyable<Mailbox>::value);
static_assert(sizeof(Span) == 16 && sizeof(Request) == 544 && sizeof(Completion) == 32);
static_assert(offsetof(Mailbox, done_sequence) % 64 == 0);
static_assert(offsetof(Mailbox, terminal) % 64 == 0);
}
