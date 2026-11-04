#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <array>
#include <cstddef>

namespace hbf_eventlab {
using AssemblySlotId = Quantity<struct AssemblySlotTag>;
using PageData = std::array<DataToken, fragments_per_dlu>;
// Data owner, separate from PendingWriteContext's lifetime/bitmap/command metadata.
class IAssemblyStorage {
public:
    virtual ~IAssemblyStorage() = default;
    virtual std::size_t capacity() const = 0;
    virtual AssemblySlotId allocate() = 0;
    virtual void write(AssemblySlotId, std::uint32_t slot, DataToken) = 0;
    virtual const PageData& page(AssemblySlotId) const = 0;
    virtual void release(AssemblySlotId) = 0;
};
} // namespace hbf_eventlab
