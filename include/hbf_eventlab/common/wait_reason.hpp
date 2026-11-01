#pragma once
#include <cstddef>
namespace hbf_eventlab {
// Shared by notification keys and optional aggregate software diagnostics.
enum class WaitReason : std::size_t { Credit, BlockProgress, LogicalReaders, PhysicalUsers, Zone,
    DluProgress, ReadOrder, ResultSpace, ResponseReady, IngressSpace, RequestLink, Bank, CacheSpace, SenseOrder,
    PhysicalState, ZoneUsers, Count };
} // namespace hbf_eventlab
