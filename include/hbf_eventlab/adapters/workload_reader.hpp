#pragma once
#include "hbf_eventlab/host/workload.hpp"
#include <istream>
namespace hbf_eventlab {
// Text adapter is a separate library; core Host accepts typed requests, not files.
std::vector<GlobalRequest> read_workload(std::istream&);
}

