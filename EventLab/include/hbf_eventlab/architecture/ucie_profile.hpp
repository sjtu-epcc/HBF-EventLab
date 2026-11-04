#pragma once
#include "hbf_eventlab/common/types.hpp"
namespace hbf_eventlab {
enum class UcieSpeedGrade : std::uint32_t { Grade1=1, Grade2=2, Grade3=3 };
struct UcieSpeedProfile {
    std::uint32_t gt_per_second;
    std::uint64_t effective_gbytes_per_second;
    std::uint32_t max_modules, max_stack_height;
};
// OCP HBF v0.7.0 sections 5.1.2.1/2/3. Limits are maxima, not actual counts.
// Use the section's explicit 48/94/188 values, not a recomputed raw-rate ratio.
const UcieSpeedProfile& ucie_speed_profile(UcieSpeedGrade);
}
