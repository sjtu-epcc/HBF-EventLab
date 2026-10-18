#include "hbf_eventlab/architecture/ucie_profile.hpp"
namespace hbf_eventlab {
const UcieSpeedProfile& ucie_speed_profile(UcieSpeedGrade grade) {
    static constexpr UcieSpeedProfile profiles[]={{8,48,8,8},{16,94,16,16},{32,188,16,16}};
    const auto n=static_cast<std::uint32_t>(grade);
    if(n<1 || n>3) throw std::invalid_argument("device.ucie_speed_grade: expected 1, 2, or 3");
    return profiles[n-1];
}
}
