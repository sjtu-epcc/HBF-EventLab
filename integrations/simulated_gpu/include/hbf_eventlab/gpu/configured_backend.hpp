#pragma once
#include "hbf_eventlab/configuration/simulation_configuration.hpp"
#include "hbf_eventlab/gpu/page_interleaver.hpp"
#include <fstream>

namespace hbf_eventlab {
// Shared by the live Accel-Sim bridge and the standalone adapter smoke test.
// GPU sector routing packs complete pages into this zero-based backend space.
inline ResolvedSimulationConfiguration resolve_gpu_backend(SimulationConfiguration c) {
    auto resolved=resolve_configuration(std::move(c));
    const auto& v=resolved.values();
    if(v.address.global_base.value || v.address.interleave_bytes!=4096)
        throw std::invalid_argument("GPU page routing requires address.global_base=0 and interleave_bytes=4096");
    // These CLI-owned output consumers have not been installed by this adapter.
    // Reject them instead of publishing an effective configuration that is ignored.
    const auto& o=v.output;
    if(o.trace || !o.statistics_json.empty() || !o.statistics_events_jsonl.empty() || !o.runtime_profile_json.empty())
        throw std::invalid_argument("GPU adapter supports output.effective_config; use HBF_EVENTLAB_TRACE/HBF_DIAG_OUTPUT for adapter diagnostics");
    return resolved;
}
inline ResolvedSimulationConfiguration read_gpu_backend(const std::string& path) {
    std::ifstream in(path);
    if(!in)throw std::invalid_argument("cannot open EventLab configuration: "+path);
    return resolve_gpu_backend(read_configuration(in,path));
}
class ConfiguredGpuBackend {
public:
    ResolvedSimulationConfiguration configuration;
    EventQueue& events;
    DeviceDriver device;
    GpuPageInterleaver interleaver;
    HostDriver host;
    ConfiguredGpuBackend(ResolvedSimulationConfiguration c,EventQueue& e)
        :configuration(std::move(c)),events(e),
         device(configuration.description(),configuration.values().execution,e),
         interleaver(configuration.values().device.geometry.host_channels,
                     configuration.values().device.geometry.channel_bytes()),
         host(device,e,interleaver,configuration.values().host) {
        events.set_limits(configuration.values().run.events);
        device.set_program_outcome(make_program_outcome(configuration.values().faults));
    }
    void check_time(Tick t) const {
        const auto maximum=configuration.values().run.max_tick;
        if(maximum && *maximum<t)throw EventLimitReached("GPU backend run.max_tick_ps reached (includes preload)");
    }
    void drain_events() {
        while(auto t=events.next_time()){check_time(*t);events.run_next();}
    }
    void advance_open(Tick t) {
        check_time(t);
        while(auto next=events.next_time()) {
            if(!(*next<t))break;
            events.run_next();
        }
        events.advance_idle_to(t); // same-tick arrivals remain legal
    }
};
}

