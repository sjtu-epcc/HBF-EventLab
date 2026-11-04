#pragma once
#include "hbf_eventlab/common/host_protocol.hpp"
#include "hbf_eventlab/architecture/device_description.hpp"
#include "hbf_eventlab/runtime/admission_wait_registry.hpp"
#include "hbf_eventlab/runtime/read_order_tracker.hpp"
#include <map>
#include <optional>
#include <vector>

namespace hbf_eventlab {
struct CommandFragment {
    CommandId parent;
    DluKey key;
    std::uint32_t slot, index_in_command, fragments_in_command;
};
// One CHANNEL-LOCAL command. HostDriver performs workload-level cross-channel splitting.
// A burst retains ONE parent ID; the ledger counts the parent, not these fragments.
enum class CommandError { InvalidAddress, InvalidUserField };
struct CommandNormalization {
    std::optional<CommandError> error;
    std::vector<CommandFragment> fragments;
};
// Call AFTER the MOCS gate. Only modeled Host errors become protocol responses.
// Unsupported API shapes/profiles remain diagnostics, not invented user-field errors.
CommandNormalization inspect_command(const HostCommand&, const StaticDeviceDescription&);
// Diagnostic adapter retained for component tests and workload normalization.
std::vector<CommandFragment> normalize_command(const HostCommand&, const StaticDeviceDescription&);
enum class AdmissionMode { Backpressure, StrictInputValidation };
enum class SubmitResult { Accepted, Backpressured };
class CommandAdmission {
public:
    CommandAdmission(std::uint32_t channels, std::uint32_t limit,
                     AdmissionWaitRegistry&, AdmissionMode mode = AdmissionMode::Backpressure);
    CommandAdmission(const CommandAdmission&) = delete;
    CommandAdmission& operator=(const CommandAdmission&) = delete;
    SubmitResult try_accept(const HostCommand&);
    bool has_credit(ChannelId) const;
    bool contains(ChannelId, CommandId) const;
    std::size_t outstanding(ChannelId) const;
    std::size_t channels() const { return accepted_.size(); }
    std::uint32_t limit() const { return limit_; }
    AdmissionWaitRegistry& notifications() const { return notifications_; }
    ReadOrderTracker& read_order() { return read_order_; }
    std::size_t total_outstanding() const;
    // Called ONCE when success OR error completion is delivered to Host.
    void response_delivered(ChannelId, CommandId);
private:
    friend class WritePath;
    friend class ReadPath;
    // Object teardown only: not a Host completion or a runtime cancellation API.
    void abandon_on_shutdown(ChannelId ch, CommandId id) noexcept { accepted_[ch].erase(id); }
    std::uint32_t limit_;
    AdmissionMode mode_;
    AdmissionWaitRegistry& notifications_;
    ReadOrderTracker read_order_;
    std::vector<std::map<CommandId, HostCommand>> accepted_;
};
} // namespace hbf_eventlab
