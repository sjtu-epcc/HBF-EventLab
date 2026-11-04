#pragma once
#include "hbf_eventlab/common/host_protocol.hpp"
#include "hbf_eventlab/architecture/device_description.hpp"
#include "hbf_eventlab/interfaces/address.hpp"
#include "hbf_eventlab/runtime/admission_wait_registry.hpp"
#include <vector>
#include <variant>
namespace hbf_eventlab {
struct LocalTransaction { HostCommand command; std::vector<DataToken> data; };
using DeviceResponse=std::variant<ReadResponse,WriteResponse>;
struct DeliveredResponse { TransferAttemptId attempt; DeviceResponse response; Tick at; };
enum class HandoffDisposition { NotTaken, Taken };
struct HandoffResult {
    HandoffDisposition disposition;
    std::optional<TransferAttemptId> attempt;
    std::vector<WaitKey> waits;
};
struct AcceptedNotice { TransferAttemptId attempt; ChannelId channel; CommandId command; Tick at; };
// Public transaction-level progress only; not a new AXI message or completion.
// All callbacks are asynchronous on notifications().events(). Taken retains the
// attempt until final response. Exactly one acceptance notice (if supplied) must
// precede exactly one final response, including command-level errors. NotTaken
// produces neither callback; its wait keys denote possible retries, not success.
// A port/notification registry must outlive its Host; no inline callback/reentrancy.
class IHostPort {
public:
    using Receiver=std::function<void(const DeliveredResponse&)>;
    using AcceptanceObserver=std::function<void(const AcceptedNotice&)>;
    virtual ~IHostPort()=default;
    // Advisory, non-reserving query for blockers shared by EVERY candidate on
    // this channel. Empty means "no known uniform blocker", not a grant:
    // try_submit remains authoritative for transaction-specific conditions.
    // Observe each wait key BEFORE inspecting its condition; returned revisions
    // must be passed unchanged to subscribe to avoid check/register lost wakeups.
    // Existing/multi-lane ports may leave this unimplemented and use try_submit.
    virtual std::vector<WaitObservation> channel_send_blockers(ChannelId) const { return {}; }
    virtual HandoffResult try_submit(const LocalTransaction&,Receiver,AcceptanceObserver = {})=0;
    virtual AdmissionWaitRegistry& notifications()=0;
    virtual const StaticDeviceDescription& description() const=0;
    virtual const IAddressLayout& layout() const=0;
};
} // namespace hbf_eventlab
