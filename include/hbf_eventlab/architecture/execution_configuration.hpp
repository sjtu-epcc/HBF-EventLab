#pragma once
#include "hbf_eventlab/architecture/device_description.hpp"
#include "hbf_eventlab/architecture/ucie_profile.hpp"
#include <string>
namespace hbf_eventlab {
// Fixed execution time base, not an adjustable hardware parameter.
inline constexpr std::uint64_t picoseconds_per_tick=1;
struct BufferCapacity {
    std::size_t entries;
    std::size_t payload_bytes; // payload only; control metadata consumes an entry
};
struct BufferConfiguration {
    // Model capacities per UCIe channel, NOT mandated adapter depths.
    BufferCapacity ingress{16,16384}, egress{8,512};
};
struct MediaTimingConfiguration {
    // Illustrative, uncalibrated durations. Tick = 1 ps.
    Tick sense{100000}, program{500000}, erase{1000000}, cache_access{100};
};
struct LinkOverheadConfiguration {
    // Model service-byte equivalents, NOT literal AXI packet sizes.
    std::uint64_t request_control_bytes=16;
    std::uint64_t response_control_bytes=16;
    // May be zero; additional delay after serialization, without holding link BW.
    Tick request_fixed_latency{100}, response_fixed_latency{100};
};
struct UcieExecutionConfiguration {
    UcieSpeedGrade speed_grade=UcieSpeedGrade::Grade1;
    LinkOverheadConfiguration overhead;
    std::uint64_t effective_gbytes_per_second() const { return ucie_speed_profile(speed_grade).effective_gbytes_per_second; }
    Tick serialization(std::uint64_t service_bytes) const;
};
struct ReadDispatchConfiguration {
    std::string policy="window";
    Tick wait{10000}; // 0 releases without an accumulation delay
    std::size_t threshold=32; // 0 disables count trigger; timer/Batch remain active
};
struct ExecutionConfiguration {
    MediaTimingConfiguration media_timing;
    UcieExecutionConfiguration ucie;
    BufferConfiguration buffers;
    ReadDispatchConfiguration read_dispatch;
    std::string nand_scheduler="fcfs";
    std::string ucie_scheduler="fifo";
    void validate(const StaticDeviceDescription&) const;
};
} // namespace hbf_eventlab
