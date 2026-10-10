#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace hbf_eventlab {
template<class Tag> struct Quantity {
    std::uint64_t value = 0;
    constexpr Quantity() = default;
    explicit constexpr Quantity(std::uint64_t v) : value(v) {}
    friend constexpr bool operator==(Quantity a, Quantity b) { return a.value == b.value; }
    friend constexpr bool operator!=(Quantity a, Quantity b) { return !(a == b); }
    friend constexpr bool operator<(Quantity a, Quantity b) { return a.value < b.value; }
};
using Tick = Quantity<struct TickTag>;
using CommandId = Quantity<struct CommandIdTag>;
using DataToken = Quantity<struct DataTokenTag>;
using GlobalByteAddress = Quantity<struct GlobalByteTag>;
// Zero-based BYTE OFFSET inside a channel Flash logical region, not an AXI port address.
// OCP Figure 3 instead uses 64B units; normalize any external base exactly once at ingress.
using LocalByteAddress = Quantity<struct LocalByteTag>;
using LocalAddress64B = Quantity<struct Local64BTag>;
using LocalDlu = Quantity<struct LocalDluTag>;
using MediaDlu = Quantity<struct MediaDluTag>;
using Generation = Quantity<struct GenerationTag>;
using EventId = Quantity<struct EventIdTag>;
using OperationId = Quantity<struct OperationTag>;
// Separate ticket domain prevents accidentally completing a read as an erase/program.
using ReadOperationId = Quantity<struct ReadOperationTag>;
using ChannelId = std::uint32_t;
using PageId = std::uint32_t;
constexpr std::uint32_t host_unit_bytes = 64;
constexpr std::uint32_t dlu_bytes = 4096;
constexpr std::uint32_t fragments_per_dlu = dlu_bytes / host_unit_bytes;
inline std::uint64_t checked_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) throw std::overflow_error("addition overflow");
    return a + b;
}
inline std::uint64_t checked_mul(std::uint64_t a, std::uint64_t b) {
    if (a && b > std::numeric_limits<std::uint64_t>::max() / a) throw std::overflow_error("multiplication overflow");
    return a * b;
}
struct ChannelAddress { ChannelId channel; LocalByteAddress local; };
struct MediaAddress { ChannelId channel; MediaDlu dlu; std::uint32_t byte_offset; };
struct DluKey {
    ChannelId channel;
    LocalDlu dlu;
    friend bool operator<(const DluKey& a, const DluKey& b) {
        return std::tie(a.channel, a.dlu) < std::tie(b.channel, b.dlu);
    }
};
struct LogicalBlockKey {
    ChannelId channel;
    std::uint32_t die, bank;
    std::uint64_t block;
    friend bool operator<(const LogicalBlockKey& a, const LogicalBlockKey& b) {
        return std::tie(a.channel, a.die, a.bank, a.block) < std::tie(b.channel, b.die, b.bank, b.block);
    }
    friend bool operator==(const LogicalBlockKey& a, const LogicalBlockKey& b) { return !(a < b) && !(b < a); }
};
struct LogicalNandAddress { LogicalBlockKey block; PageId page; std::uint32_t byte_offset; };
// Layout output is a nominal media slot; only replacement resolves actual silicon.
struct NandSlotAddress {
    ChannelId channel;
    std::uint32_t die, bank;
    std::uint64_t block;
    PageId page;
    std::uint32_t byte_offset;
};
template<class Tag> struct BlockCoordinates {
    ChannelId channel;
    std::uint32_t die, bank;
    std::uint64_t block;
    friend bool operator<(const BlockCoordinates& a, const BlockCoordinates& b) {
        return std::tie(a.channel, a.die, a.bank, a.block) < std::tie(b.channel, b.die, b.bank, b.block);
    }
    friend bool operator==(const BlockCoordinates& a, const BlockCoordinates& b) { return !(a < b) && !(b < a); }
    friend bool operator!=(const BlockCoordinates& a, const BlockCoordinates& b) { return !(a == b); }
};
using BlockSlotKey = BlockCoordinates<struct SlotTag>;
using PhysicalBlockKey = BlockCoordinates<struct PhysicalTag>;
struct PhysicalNandAddress {
    ChannelId channel;
    std::uint32_t die, bank;
    std::uint64_t block;
    PageId page;
    std::uint32_t byte_offset;
};
inline BlockSlotKey block_of(const NandSlotAddress& a) { return {a.channel, a.die, a.bank, a.block}; }
inline PhysicalBlockKey block_of(const PhysicalNandAddress& a) { return {a.channel, a.die, a.bank, a.block}; }
inline LocalByteAddress to_bytes(LocalAddress64B a) { return LocalByteAddress{checked_mul(a.value, host_unit_bytes)}; }
inline LocalAddress64B to_units64(LocalByteAddress a) {
    if (a.value % host_unit_bytes) throw std::invalid_argument("unaligned local byte offset");
    return LocalAddress64B{a.value / host_unit_bytes};
}
enum class Op { Read, Write };
enum class ReadHint { Regular, Batch };
// Implemented response subsets, not a complete AXI codec. Read/write values differ.
enum class WriteStatus : std::uint8_t {
    Success = 0, InvalidAddress = 1, OverlapAddress = 2, InvalidUserField = 3, PendingDluLimit = 4,
    AssemblyTimeout = 5, WriteOrderViolation = 6, ProgramFail = 7, CapacityUnusable = 8
};
enum class ReadStatus : std::uint8_t {
    Success = 0, InvalidAddress = 1, TemporarilyRestricted = 2, InvalidUserField = 3,
    UnwrittenPage = 7, PendingWriteMissing = 0xA
};
} // namespace hbf_eventlab
