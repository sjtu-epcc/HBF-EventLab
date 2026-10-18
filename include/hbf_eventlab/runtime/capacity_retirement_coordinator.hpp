#pragma once
#include "hbf_eventlab/runtime/address_resolver.hpp"
#include "hbf_eventlab/runtime/nand_media_state.hpp"
#include "hbf_eventlab/runtime/retired_hole_map.hpp"
#include "hbf_eventlab/runtime/logical_access_tracker.hpp"
#include "hbf_eventlab/runtime/admission_wait_registry.hpp"
#include <set>

namespace hbf_eventlab {
enum class CapacityResult { Ready, Replaced, InFlight, Retired, AlreadyRetired };
// Single simulator-thread transaction owner. Dependencies must outlive it; it
// must outlive WritePath/AddressResolver. Attached low-level mutations are blocked.
class CapacityRetirementCoordinator {
public:
    CapacityRetirementCoordinator(const StaticDeviceDescription&, const IAddressLayout&,
                                 ZoneMapping&, BlockReplacement&, NandMediaState&, CapacityPolicy = {});
    ~CapacityRetirementCoordinator();
    CapacityRetirementCoordinator(const CapacityRetirementCoordinator&) = delete;
    CapacityRetirementCoordinator& operator=(const CapacityRetirementCoordinator&) = delete;
    bool retired(LogicalBlockKey) const;
    const std::set<LogicalBlockKey>& retired_blocks() const { return retired_; }
    const RetiredHoleMap& holes() const { return holes_; }
    std::uint64_t hole_limit(ChannelId) const;
    Generation generation() const { return generation_; }
    Generation content_generation(LogicalBlockKey) const;
    NandSlotAddress slot(ChannelAddress) const;
    PhysicalNandAddress inspect(ChannelAddress) const;
    std::shared_ptr<ResolvedAddressLease> pin(ChannelAddress);
    std::optional<LogicalReadId> acquire_read(ChannelAddress);
    void release_read(LogicalReadId, bool notify = true);
    std::shared_ptr<ResolvedAddressLease> pin_read(ChannelAddress, LogicalReadId);
    const LogicalAccessTracker& logical_reads() const { return logical_reads_; }
    bool quiesced(ChannelAddress a) const { return zones_.quiesced(a); }
    WaitKey zone_wait(ChannelAddress a) const;
    void attach_notifications(AdmissionWaitRegistry&);
    // Lease destructors only mark dirty objects. Driver/path publishes at a stable boundary.
    void publish_releases();
    std::uint64_t in_flight(PhysicalBlockKey) const;
    void mark_unusable(PhysicalBlockKey);
    CapacityResult repair_block(LogicalBlockKey, const std::function<void()>& invalidate);
    void declare_discardable(ChannelId, std::uint64_t zone);
    bool quiesce_pair(ChannelId, std::uint64_t, std::uint64_t);
    void cancel_quiesce(ChannelId, std::uint64_t, std::uint64_t);
    RemapResult commit_swap(ChannelId, std::uint64_t, std::uint64_t, const std::function<void()>& invalidate);
    // Diagnostics permit known failed targets awaiting repair, but never aliases.
    void validate_invariants(bool require_healthy = false) const;
private:
    void require_idle() const;
    ChannelAddress address(LogicalBlockKey) const;
    std::uint64_t zone_of(LogicalBlockKey) const;
    std::vector<LogicalBlockKey> blocks(ChannelId) const;
    PhysicalNandAddress resolve_unchecked(ChannelAddress) const;
    std::shared_ptr<ResolvedAddressLease> pin_impl(ChannelAddress, bool existing_reader);
    void notify_zone(ChannelId, std::uint64_t);
    const StaticDeviceDescription& description_;
    const IAddressLayout& layout_;
    ZoneMapping& zones_;
    BlockReplacement& replacement_;
    NandMediaState& media_;
    CapacityPolicy policy_;
    RetiredHoleMap holes_;
    std::set<LogicalBlockKey> retired_;
    std::map<std::pair<ChannelId, std::uint64_t>, Generation> contents_;
    std::shared_ptr<PhysicalLeaseState> leases_;
    Generation generation_;
    bool busy_ = false;
    LogicalAccessTracker logical_reads_;
    AdmissionWaitRegistry* notifications_ = nullptr;
};
} // namespace hbf_eventlab
