#pragma once
#include "hbf_eventlab/architecture/device_description.hpp"
#include "hbf_eventlab/interfaces/address.hpp"
#include <functional>
#include <memory>

namespace hbf_eventlab {
struct ReplacementState;
class ReplacementLease {
public:
    ReplacementLease(const ReplacementLease&) = delete;
    ReplacementLease& operator=(const ReplacementLease&) = delete;
    ReplacementLease(ReplacementLease&&) noexcept;
    ~ReplacementLease();
    PhysicalNandAddress physical() const { return physical_; }
private:
    friend class BlockReplacement;
    ReplacementLease(std::shared_ptr<ReplacementState>, BlockSlotKey, PhysicalNandAddress);
    std::shared_ptr<ReplacementState> state_;
    BlockSlotKey slot_;
    PhysicalNandAddress physical_;
};
enum class ReplacementResult { Applied, InFlight, NoSpare };
// REFERENCE fault-replacement mechanism, NOT the OCP Zone Remapping command.
// Spares stay in the same channel/die/bank and are never reused after retirement.
class BlockReplacement final : public IBlockReplacement {
public:
    explicit BlockReplacement(Geometry);
    BlockReplacement(const BlockReplacement&) = delete;
    BlockReplacement& operator=(const BlockReplacement&) = delete;
    PhysicalNandAddress translate(const NandSlotAddress&) const override;
    ReplacementLease pin(const NandSlotAddress&);
    std::uint64_t in_flight(BlockSlotKey) const;
    bool retired(PhysicalBlockKey) const;
    // Caller has diagnosed an unusable target and drained old operations.
    // Invalidation is synchronous/non-reentrant, and must leave caches unchanged on throw.
    ReplacementResult replace_failed(BlockSlotKey, const std::function<void()>& invalidate);
private:
    std::shared_ptr<ReplacementState> state_;
    friend class CapacityRetirementCoordinator;
    struct Plan { ReplacementResult result; std::shared_ptr<ReplacementState> state; };
    Plan plan_replace(BlockSlotKey, PhysicalBlockKey, const std::function<bool(PhysicalBlockKey)>&) const;
    void commit(Plan& plan) noexcept { state_.swap(plan.state); }
    ReplacementLease pin_impl(const NandSlotAddress&);
    bool managed_ = false;
    bool has_pins() const;
};
} // namespace hbf_eventlab
