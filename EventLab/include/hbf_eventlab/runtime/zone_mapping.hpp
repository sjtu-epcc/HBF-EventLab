#pragma once
#include "hbf_eventlab/architecture/device_description.hpp"
#include "hbf_eventlab/interfaces/address.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace hbf_eventlab {
struct MappingState;
// Keeps the old mapping pinned until the operation (including fills/programs) ends.
// Its shared state may outlive the remapper object. Not thread-safe: simulator thread only.
class MappingLease {
public:
    MappingLease(const MappingLease&) = delete;
    MappingLease& operator=(const MappingLease&) = delete;
    MappingLease(MappingLease&&) noexcept;
    MappingLease& operator=(MappingLease&&) = delete;
    ~MappingLease();
    MediaAddress media() const { return media_; }
    Generation generation() const { return generation_; }
private:
    friend class ZoneMapping;
    MappingLease(std::shared_ptr<MappingState>, ChannelId, std::uint64_t, MediaAddress, Generation);
    std::shared_ptr<MappingState> state_;
    ChannelId channel_;
    std::uint64_t zone_;
    MediaAddress media_;
    Generation generation_;
};
enum class RemapResult { Applied, NotQuiesced, InFlight, NotDiscardable,
                         DensityLimit, RecoveryRequired, NoCompatibleTarget };
class ZoneMapping final : public IMediaMapping {
public:
    explicit ZoneMapping(const StaticDeviceDescription&);
    ZoneMapping(const ZoneMapping&) = delete;
    ZoneMapping& operator=(const ZoneMapping&) = delete;
    // Inspection only. Actual IO must pin() so remap cannot race a retained address.
    MediaAddress translate(ChannelAddress) const override;
    Generation generation(ChannelId) const override;
    std::optional<MappingLease> pin(ChannelAddress);
    void declare_discardable(ChannelId, std::uint64_t zone);
    bool quiesce_pair(ChannelId, std::uint64_t a, std::uint64_t b);
    void cancel_quiesce(ChannelId, std::uint64_t a, std::uint64_t b);
    // Invalidation runs before the atomic swap while BOTH zones reject new IO.
    // Callback must be synchronous and non-reentrant. Throw leaves old mapping/quiesce intact.
    RemapResult commit_swap(ChannelId, std::uint64_t a, std::uint64_t b,
                           const std::function<void()>& invalidate_caches);
    std::uint64_t mapped_zone(ChannelId, std::uint64_t zone) const;
private:
    std::shared_ptr<MappingState> state_;
    friend class CapacityRetirementCoordinator;
    void require_unmanaged() const;
    std::optional<MappingLease> pin_impl(ChannelAddress, bool existing_logical_reader = false);
    bool begin_logical_use(ChannelAddress);
    void declare_discardable_impl(ChannelId, std::uint64_t);
    bool quiesce_pair_impl(ChannelId, std::uint64_t, std::uint64_t);
    void cancel_quiesce_impl(ChannelId, std::uint64_t, std::uint64_t);
    RemapResult swap_status(ChannelId, std::uint64_t, std::uint64_t) const;
    RemapResult commit_swap_impl(ChannelId, std::uint64_t, std::uint64_t,
                                 const std::function<void()>&);
    bool managed_ = false;
    bool has_pins() const;
    bool quiesced(ChannelAddress) const;
};
} // namespace hbf_eventlab
