#pragma once
#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include <istream>
#include <ostream>
#include <utility>

namespace hbf_eventlab {
struct DeviceConfiguration {
    std::string spec_profile="ocp_hbf_v0.7.0";
    Geometry geometry{2,1,1,8,4,1}; // compact functional default, not product geometry
    Capabilities capabilities{256,8,false,true};
    // Input-only zone size 0 means auto (one block group). Resolved before use.
    DeviceOperatingConfiguration selected{256,Tick{10000000},0};
    CoreChannelTopologyConfiguration topology;
    CapacityPolicy capacity_policy;
};
struct AddressConfiguration {
    GlobalByteAddress global_base{};
    std::uint64_t interleave_bytes=host_unit_bytes;
};
struct FaultConfiguration {
    LogicalBlockKey target{0,0,0,0};
    std::vector<PageId> fail_sequence;
    std::optional<PageId> persistent_page;
};
struct OutputConfiguration {
    bool trace=false;
    std::string statistics_json;
    std::string effective_config;
};
struct SimulationConfiguration {
    DeviceConfiguration device;
    AddressConfiguration address;
    HostConfiguration host;
    ExecutionConfiguration execution;
    FaultConfiguration faults;
    RunLimits run;
    OutputConfiguration output;
};

// Default -> one INI file -> explicit overrides -> resolve. Parsing does not
// instantiate runtime components. No silent unknown fields or unknown policies.
SimulationConfiguration read_configuration(std::istream&,const std::string& source="configuration");
void apply_configuration_override(SimulationConfiguration&,const std::string& key,const std::string& value);

class ResolvedSimulationConfiguration {
public:
    const SimulationConfiguration& values() const { return values_; }
    const StaticDeviceDescription& description() const { return description_; }
private:
    friend ResolvedSimulationConfiguration resolve_configuration(SimulationConfiguration);
    explicit ResolvedSimulationConfiguration(SimulationConfiguration c,StaticDeviceDescription d)
        :values_(std::move(c)),description_(std::move(d)) {}
    SimulationConfiguration values_;
    StaticDeviceDescription description_;
};
ResolvedSimulationConfiguration resolve_configuration(SimulationConfiguration);
// Output is reloadable INI; read-only derived quantities are emitted as comments.
void write_effective_configuration(std::ostream&,const ResolvedSimulationConfiguration&);
void write_configuration_summary(std::ostream&,const ResolvedSimulationConfiguration&);
// Diagnostics do not reject legal overload/timeout experiments or promise progress.
std::vector<std::string> diagnose_workload(const ResolvedSimulationConfiguration&,const std::vector<GlobalRequest>&);
std::function<ProgramOutcome(const ProgramOperation&)> make_program_outcome(const FaultConfiguration&);
}
