#pragma once
#include "hbf_eventlab/host/host_driver.hpp"
#include "hbf_eventlab/runtime/device_driver.hpp"
namespace hbf_eventlab {
enum class RunStatus { Completed, Stalled, LimitReached, ModelError, ImplementationError, Paused, WaitingForInput };
struct RunLimits {
    std::optional<Tick> max_tick=Tick{1000000000000ULL}; // 1 simulated second, not wall time
    EventLimits events{10000000,100000,100000};
};
struct RunReport {
    RunStatus status;
    bool workload_success;
    Tick at;
    std::uint64_t events;
    HostActivity host;
    DeviceActivity device;
    std::string explanation;
};
const char* to_string(RunStatus);
class SimulationRunner {
public:
    SimulationRunner(EventQueue& e,HostDriver& h,const DeviceDriver& d)
        :SimulationRunner(e,h,[&d]{return d.activity();}) {}
    SimulationRunner(EventQueue& e,HostDriver& h,std::function<DeviceActivity()> d)
        :events_(e),host_(h),device_(std::move(d)) {}
    RunReport run(RunLimits={});
private:
    RunReport report(RunStatus,const std::string&);
    EventQueue& events_;
    HostDriver& host_;
    std::function<DeviceActivity()> device_;
};
}

