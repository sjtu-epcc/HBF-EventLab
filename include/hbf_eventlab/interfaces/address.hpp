#pragma once
#include "hbf_eventlab/common/types.hpp"

namespace hbf_eventlab {
class IHostInterleaver {
public:
    virtual ~IHostInterleaver() = default;
    virtual ChannelAddress map(GlobalByteAddress) const = 0;
};
class IAddressLayout {
public:
    virtual ~IAddressLayout() = default;
    virtual LogicalNandAddress logical_view(ChannelAddress) const = 0;
    virtual LocalByteAddress encode_logical(const LogicalNandAddress&) const = 0;
    virtual NandSlotAddress decode(MediaAddress) const = 0;
};
class IMediaMapping {
public:
    virtual ~IMediaMapping() = default;
    virtual MediaAddress translate(ChannelAddress) const = 0;
    virtual Generation generation(ChannelId) const = 0;
};
class IBlockReplacement {
public:
    virtual ~IBlockReplacement() = default;
    virtual PhysicalNandAddress translate(const NandSlotAddress&) const = 0;
};
} // namespace hbf_eventlab
