#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <optional>

namespace hbf_eventlab {
using ReadGroup = std::uint32_t;
// Public command/response values, independent of path implementation classes.
struct HostCommand {
    CommandId id;
    Op op;
    ChannelAddress address;
    std::uint32_t size_bytes;
    std::uint16_t axi_id;
    ReadHint read_hint = ReadHint::Regular;
    std::optional<ReadGroup> read_group;
};
enum class ReadRestrictionReason { None, AwaitingReplay, ReplayInProgress, PhysicalRepairRequired };
struct ReadResponse {
    ChannelId channel;
    CommandId command;
    ReadStatus status;
    std::optional<DataToken> data; // Observational copy; authoritative payload has a bounded owner.
    ReadRestrictionReason restriction = ReadRestrictionReason::None;
};
struct WriteResponse {
    ChannelId channel;
    CommandId command;
    WriteStatus status;
    std::uint8_t additional_info;
};
} // namespace hbf_eventlab
