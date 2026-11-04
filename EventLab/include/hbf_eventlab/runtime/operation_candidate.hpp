#pragma once
#include "hbf_eventlab/runtime/admission_wait_registry.hpp"

namespace hbf_eventlab {
using CandidateId = Quantity<struct CandidateTag>;
enum class ProbeState { Stale, Blocked, Ready, Terminal };
struct PathProbe { ProbeState state; std::vector<WaitKey> waits; };
struct ProgramCandidate { DluKey dlu; Generation context; };
struct EraseCandidate { CandidateId id; LogicalBlockKey logical; PhysicalBlockKey physical; };
// Read/erase candidate identity is different from an issued operation ticket.
struct ReadCandidate { ChannelId channel; CommandId command; CandidateId identity; };
} // namespace hbf_eventlab
