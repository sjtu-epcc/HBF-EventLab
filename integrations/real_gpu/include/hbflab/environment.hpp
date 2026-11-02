#pragma once
#include "hbflab/backend.hpp"

namespace hbflab {
// Explicit, uncalibrated lumped RC / temperature-accelerated retention model.
// All time is EventLab time. GPU watts are supplied by the embedding application;
// they are not inferred from kernel wall time or presented as measured telemetry.
struct EnvironmentOptions {
    bool enabled=false;
    double ambient_c=25, initial_c=25, resistance_k_per_w=0.5, capacitance_j_per_k=10;
    double gpu_coupling=0.1, idle_w=0.1, read_j_per_byte=1e-10, program_j_per_byte=1e-9;
    double reference_c=85, retention_seconds=86400, half_life_degrees=10;
    double refresh_fraction=0.8;
    std::size_t refresh_budget=1; // maximum logical page refreshes per explicit service call
};
struct EnvironmentStats {
    std::uint64_t time_ps=0, samples=0;
    double temperature_c=25, peak_c=25, media_energy_j=0, exposure=0;
};
class EnvironmentModel {
public:
    explicit EnvironmentModel(EnvironmentOptions={});
    void sample(std::uint64_t time_ps,const BackendStats&,double gpu_watts);
    EnvironmentStats statistics() const{return stats_;}
    const EnvironmentOptions& options() const{return options_;}
private:
    EnvironmentOptions options_;
    EnvironmentStats stats_;
    BackendStats previous_{};
    bool started_=false;
};
}
