#pragma once
#include "integrated_profile.hpp"
#include "hbflab/backend.hpp"
inline hbf_eventlab::Geometry m6_geometry(){auto g=hbflab::BackendOptions{}.geometry;g.pages_per_block=integrated_profile().pages_per_block;g.blocks_per_bank=integrated_profile().blocks_per_bank;return g;}
