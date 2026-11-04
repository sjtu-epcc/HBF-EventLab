#pragma once
#include "hbf_eventlab/runtime/zone_mapping.hpp"
#include "hbf_eventlab/runtime/block_replacement.hpp"
#include <utility>

namespace hbf_eventlab {
class CapacityRetirementCoordinator;
struct PhysicalLeaseState;
// Pins the actual silicon, including targets borrowed across physical zones.
class PhysicalMappingLease {
public:
    PhysicalMappingLease(const PhysicalMappingLease&) = delete;
    PhysicalMappingLease& operator=(const PhysicalMappingLease&) = delete;
    PhysicalMappingLease(PhysicalMappingLease&&) noexcept;
    ~PhysicalMappingLease();
private:
    friend class CapacityRetirementCoordinator;
    PhysicalMappingLease(std::shared_ptr<PhysicalLeaseState>, PhysicalBlockKey);
    std::shared_ptr<PhysicalLeaseState> state_;
    PhysicalBlockKey physical_;
};
class ResolvedAddressLease {
public:
    ResolvedAddressLease(MappingLease zone, ReplacementLease replacement,
                         PhysicalMappingLease physical_lease, NandSlotAddress slot,
                         PhysicalNandAddress physical, Generation contents)
        : zone_(std::move(zone)), replacement_(std::move(replacement)),
          physical_lease_(std::move(physical_lease)), slot_(slot), physical_(physical), contents_(contents) {}
    NandSlotAddress slot() const { return slot_; }
    PhysicalNandAddress physical() const { return physical_; }
    Generation content_generation() const { return contents_; }
private:
    MappingLease zone_;
    ReplacementLease replacement_;
    PhysicalMappingLease physical_lease_;
    NandSlotAddress slot_;
    PhysicalNandAddress physical_;
    Generation contents_;
};
// All actual resolution goes through the shared capacity/ownership view.
class AddressResolver {
public:
    explicit AddressResolver(CapacityRetirementCoordinator& capacity) : capacity_(capacity) {}
    NandSlotAddress slot(ChannelAddress) const;
    PhysicalNandAddress inspect(ChannelAddress) const;
    std::shared_ptr<ResolvedAddressLease> pin(ChannelAddress);
private:
    CapacityRetirementCoordinator& capacity_;
};
} // namespace hbf_eventlab
