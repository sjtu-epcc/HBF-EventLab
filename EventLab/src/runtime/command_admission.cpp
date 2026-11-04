#include "hbf_eventlab/runtime/command_admission.hpp"
#include <utility>

namespace hbf_eventlab {
CommandNormalization inspect_command(const HostCommand& cmd, const StaticDeviceDescription& d) {
    const auto& a = cmd.address;
    const auto cap = d.geometry().channel_bytes();
    if (a.channel >= d.geometry().host_channels || (cmd.op != Op::Read && cmd.op != Op::Write))
        throw std::invalid_argument("invalid simulator command route/op");
    if (cmd.op==Op::Write && cmd.read_group)
        throw std::invalid_argument("read_group is a read-only simulator input field");
    if ((cmd.read_hint != ReadHint::Regular && cmd.read_hint != ReadHint::Batch) ||
        (cmd.op == Op::Write && cmd.read_hint != ReadHint::Regular))
        return {CommandError::InvalidUserField, {}};
    if (!cmd.size_bytes || cmd.size_bytes > dlu_bytes || cmd.size_bytes % host_unit_bytes)
        throw std::invalid_argument("unsupported simulator command size");
    // Subtraction avoids wraparound near UINT64_MAX. No logical block is invented.
    if (a.local.value >= cap || cmd.size_bytes > cap - a.local.value)
        return {CommandError::InvalidAddress, {}};
    if (a.local.value % host_unit_bytes)
        throw std::invalid_argument("simulator command address must be 64B aligned");
    const auto end = a.local.value + cmd.size_bytes - 1;
    if (a.local.value / dlu_bytes != end / dlu_bytes)
        throw std::invalid_argument("baseline command must stay within one DLU");
    if (cmd.size_bytes > host_unit_bytes && !(cmd.op == Op::Read ? d.capabilities().read_burst : d.capabilities().write_burst))
        throw std::invalid_argument("burst unsupported by profile");
    // Reference write burst is a full aligned DLU; partial write bursts are not modelled.
    if (cmd.op == Op::Write && cmd.size_bytes > host_unit_bytes &&
        (cmd.size_bytes != dlu_bytes || a.local.value % dlu_bytes))
        throw std::invalid_argument("baseline supports 64B writes or aligned 4KiB write bursts");
    std::vector<CommandFragment> out;
    const auto count = cmd.size_bytes / host_unit_bytes;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        out.push_back({cmd.id, {a.channel, LocalDlu{a.local.value / dlu_bytes}},
            static_cast<std::uint32_t>(a.local.value % dlu_bytes / host_unit_bytes) + i, i, count});
    return {std::nullopt, std::move(out)};
}
std::vector<CommandFragment> normalize_command(const HostCommand& cmd, const StaticDeviceDescription& d) {
    auto result = inspect_command(cmd, d);
    if (result.error) throw std::invalid_argument("invalid Host command address/user field");
    return std::move(result.fragments);
}
CommandAdmission::CommandAdmission(std::uint32_t channels, std::uint32_t limit, AdmissionWaitRegistry& notifications, AdmissionMode mode)
    : limit_(limit), mode_(mode), notifications_(notifications), accepted_(channels) {
    if (!channels || !limit) throw std::invalid_argument("admission dimensions must be positive");
}
SubmitResult CommandAdmission::try_accept(const HostCommand& cmd) {
    auto& channel = accepted_.at(cmd.address.channel);
    if (channel.count(cmd.id)) throw std::logic_error("duplicate outstanding command ID");
    if (channel.size() >= limit_) {
        if (mode_ == AdmissionMode::StrictInputValidation)
            throw std::runtime_error("Host input exceeds MOCS; diagnostic, NOT a protocol error");
        return SubmitResult::Backpressured;
    }
    channel.emplace(cmd.id, cmd);
    return SubmitResult::Accepted;
}
bool CommandAdmission::has_credit(ChannelId ch) const { return accepted_.at(ch).size() < limit_; }
bool CommandAdmission::contains(ChannelId ch, CommandId id) const { return accepted_.at(ch).count(id) != 0; }
std::size_t CommandAdmission::outstanding(ChannelId ch) const { return accepted_.at(ch).size(); }
void CommandAdmission::response_delivered(ChannelId ch, CommandId id) {
    if (!accepted_.at(ch).erase(id)) throw std::logic_error("unknown/already-completed command");
    notifications_.notify(WaitKey::channel_key(WaitReason::Credit,ch));
}
std::size_t CommandAdmission::total_outstanding() const {
    std::size_t n=0; for (const auto& ch:accepted_) n+=ch.size(); return n;
}
} // namespace hbf_eventlab
