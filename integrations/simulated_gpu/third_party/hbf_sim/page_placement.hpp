#pragma once
// From hbf/hbf_controller.h at 3415a3171d9312fac6a835bc1aa188f7aab685c0.
// Original function body retained; isolated namespace added. See LICENSE.
namespace hbf_sim_import {
static inline unsigned hbf_page_to_channel(unsigned num_channels, int map_mode,
                                           unsigned long long page,
                                           unsigned long long pages_per_channel) {
  if (num_channels <= 1) return 0;
  if (map_mode == 1) {
    if (pages_per_channel == 0) return num_channels - 1;
    unsigned ch = (unsigned)(page / pages_per_channel);
    return ch < num_channels ? ch : num_channels - 1;
  }
  return (unsigned)(page % num_channels);
}

} // namespace hbf_sim_import

