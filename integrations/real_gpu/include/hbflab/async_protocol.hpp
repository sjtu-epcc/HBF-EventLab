#pragma once
#include "hbflab/protocol.hpp"
namespace hbflab {
// Separate protocol; never reinterpret an ABI v2 collective mailbox as this ABI.
inline constexpr std::uint32_t async_abi_version=1;
struct AsyncRequest {
    std::uint64_t session=0, launch=0;
    Request read{};
};
struct alignas(64) AsyncMailbox {
    alignas(64) std::uint64_t want_sequence=0;
    alignas(64) std::uint64_t grant_sequence=0;
    std::uint64_t floor_ns=0;
    alignas(64) std::uint64_t request_sequence=0;
    AsyncRequest request{};
    alignas(64) std::uint64_t done_sequence=0;
    Completion completion{};
    alignas(64) std::uint32_t terminal=0;
};
struct AsyncDeviceView {
    AsyncMailbox* mailboxes=nullptr; // first slot of this launch
    const Range* ranges=nullptr;
    std::uint64_t session=0,launch=0,timeout_ns=0;
    std::uint32_t range_count=0,blocks=0,threads=0,producer_base=0;
    std::uint32_t version=async_abi_version;
};
struct AsyncAccessTrace {
    std::uint64_t wanted_ns=0,granted_ns=0,floor_ns=0;
    AccessTrace access{};
    std::uint64_t session=0,launch=0,ticket=0;
    std::uint32_t producer=0;
    std::uint64_t address=0;
    std::uint32_t bytes=0;
};
}
