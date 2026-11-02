#pragma once
#include "hbflab/live_runtime.hpp"
#include <memory>
#include <string>
// Explicit M3 resource-competition validation workload; not framework KV.
class M3CapacityProbe {
public:
    M3CapacityProbe(hbflab::LiveTimingService&,const std::string& output);
    ~M3CapacityProbe();
    void step(unsigned);
    void finish();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
