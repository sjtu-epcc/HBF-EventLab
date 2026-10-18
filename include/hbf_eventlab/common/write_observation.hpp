#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <optional>

namespace hbf_eventlab {
enum class WritePhase { Collecting, Ready, ProgramQueued, Programming };
enum class AssemblyChange { Opened, Progress, PhaseChanged, Retired };
// Value-only observation of a committed mutation. No second received bitmap,
// payload, admission decision, or modeled storage allocation belongs here.
struct AssemblyObservation {
    AssemblyChange change;
    DluKey key;
    Generation generation;
    WritePhase phase; // Last live phase for Retired.
    std::uint32_t received_fragments;
    Tick first_arrival,deadline;
    std::optional<CommandId> command;
    std::optional<WriteStatus> status;
    std::uint8_t additional_info=0;
};
} // namespace hbf_eventlab
