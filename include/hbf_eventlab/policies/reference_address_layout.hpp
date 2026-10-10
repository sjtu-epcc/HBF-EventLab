#pragma once
#include "hbf_eventlab/architecture/device_description.hpp"
#include "hbf_eventlab/interfaces/address.hpp"

namespace hbf_eventlab {
// REFERENCE interpretation: DLU [block][page][die][bank]. Other layouts require proof
// of host recovery/program-order compatibility; not all plugins are OCP compatible.
class ReferenceAddressLayout final : public IAddressLayout {
public:
    explicit ReferenceAddressLayout(Geometry);
    LogicalNandAddress logical_view(ChannelAddress) const override;
    LocalByteAddress encode_logical(const LogicalNandAddress&) const override;
    NandSlotAddress decode(MediaAddress) const override;
private:
    Geometry geometry_;
};
} // namespace hbf_eventlab
