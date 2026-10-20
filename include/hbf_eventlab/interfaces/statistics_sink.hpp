#pragma once
#include "hbf_eventlab/common/host_protocol.hpp"
#include "hbf_eventlab/common/write_observation.hpp"
#include "hbf_eventlab/interfaces/execution_policy.hpp"
#include <string>
#include <vector>
#include <variant>

namespace hbf_eventlab {
// Observation only: sinks must not change admission, scheduling, or event time.
// One sink belongs to one Host/Device run. Attach before load/first handoff and
// keep it alive until both producers are destroyed. Timestamps are event times.
struct StatGlobalDefined { GlobalRequestId id; Op op; std::uint64_t bytes; Tick release; };
struct StatWorkDefined {
    LogicalWorkId id; GlobalRequestId parent; ChannelAddress address;
    std::optional<std::uint64_t> content;
};
struct StatGlobalReleased { GlobalRequestId id; Tick at; };
struct StatWorkFinished { LogicalWorkId id; Tick at; bool success; std::string reason; };
struct StatGlobalFinished { GlobalRequestId id; Tick at; bool success; std::string reason; };
struct StatAttemptStarted { TransferAttemptId id; HostCommand command; Tick at; };
struct StatAttemptLinked {
    TransferAttemptId id; SendUnitId unit; Tick queued_at;
    std::uint64_t unit_attempt; bool replay; std::uint64_t replay_pass;
    std::optional<std::uint64_t> content;
    // Normal commands provide original IDs. Replay resolves the original write
    // footprint by (channel, local address, content); no fabricated new work IDs.
    std::vector<LogicalWorkId> work;
};
enum class AttemptMilestone { RxArrival, Accepted };
struct StatAttemptAdvanced { TransferAttemptId id; AttemptMilestone stage; Tick at; };
struct StatAttemptDelivered {
    TransferAttemptId id; Tick at; Op op; std::uint8_t status;
};
// Host verification can fail even when the device returned ReadStatus::Success.
struct StatAttemptAssessed { TransferAttemptId id; Tick at; bool success; };
struct StatPendingInitialized { ChannelId channel; std::uint32_t limit; Tick at; };
struct StatDluObserved { Tick at; AssemblyObservation observation; };
// Bank ticket identity is owned by DeviceRuntimeState; observe only successful issues.
struct StatBankOperationStarted { std::uint64_t ticket; MediaOperationKind kind; PhysicalNandAddress target; Tick at; };
struct StatBankOperationFinished { std::uint64_t ticket; Tick at; };
using StatisticsEvent=std::variant<StatGlobalDefined,StatWorkDefined,StatGlobalReleased,
    StatWorkFinished,StatGlobalFinished,StatAttemptStarted,StatAttemptLinked,
    StatAttemptAdvanced,StatAttemptDelivered,StatAttemptAssessed,StatPendingInitialized,StatDluObserved,StatBankOperationStarted,StatBankOperationFinished>;
class IStatisticsSink {
public:
    virtual ~IStatisticsSink()=default;
    virtual void observe(const StatisticsEvent&)=0;
};
} // namespace hbf_eventlab
