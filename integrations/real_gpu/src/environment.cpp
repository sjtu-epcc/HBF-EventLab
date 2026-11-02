#include "hbflab/environment.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hbflab {
EnvironmentModel::EnvironmentModel(EnvironmentOptions o):options_(o) {
    for(auto x:{o.ambient_c,o.initial_c,o.resistance_k_per_w,o.capacitance_j_per_k,o.gpu_coupling,
                o.idle_w,o.read_j_per_byte,o.program_j_per_byte,o.reference_c,o.retention_seconds,
                o.half_life_degrees,o.refresh_fraction})
        if(!std::isfinite(x))throw std::invalid_argument("nonfinite environment parameter");
    if(o.ambient_c< -100 || o.ambient_c>300 || o.initial_c< -100 || o.initial_c>300 ||
       o.resistance_k_per_w<=0 || o.capacitance_j_per_k<=0 || o.gpu_coupling<0 || o.gpu_coupling>1 ||
       o.idle_w<0 || o.read_j_per_byte<0 || o.program_j_per_byte<0 || o.retention_seconds<=0 ||
       o.half_life_degrees<=0 || o.refresh_fraction<=0 || o.refresh_fraction>=1 ||
       !o.refresh_budget || o.refresh_budget>65536)
        throw std::invalid_argument("invalid environment parameters");
    stats_.temperature_c=stats_.peak_c=o.initial_c;
}
void EnvironmentModel::sample(std::uint64_t t,const BackendStats& b,double watts) {
    if(!std::isfinite(watts) || watts<0)throw std::invalid_argument("invalid GPU power sample");
    if(started_ && (t<stats_.time_ps || b.sectors<previous_.sectors || b.replayed_pages<previous_.replayed_pages))
        throw std::invalid_argument("environment clock/counters regressed");
    if(!started_){started_=true;stats_.time_ps=t;previous_=b;return;}
    const double dt=static_cast<double>(t-stats_.time_ps)*1e-12;
    const double energy=static_cast<double>(b.sectors-previous_.sectors)*64*options_.read_j_per_byte+
        static_cast<double>(b.replayed_pages-previous_.replayed_pages)*4096*options_.program_j_per_byte;
    if(!std::isfinite(energy) || !std::isfinite(stats_.media_energy_j+energy))throw std::overflow_error("environment energy overflow");
    double temperature=stats_.temperature_c, exposure=stats_.exposure;
    if(options_.enabled) {
        // Exact RC endpoint for interval-average power; trapezoid approximation
        // of temperature acceleration between samples. Sampling cadence is part
        // of the experiment and must be recorded with model parameters.
        if(dt>0) {
            const double power=options_.idle_w+watts*options_.gpu_coupling+energy/dt;
            const double equilibrium=options_.ambient_c+power*options_.resistance_k_per_w;
            const double alpha=-std::expm1(-dt/(options_.resistance_k_per_w*options_.capacitance_j_per_k));
            temperature+=(equilibrium-temperature)*alpha;
        } else temperature+=energy/options_.capacitance_j_per_k;
        const auto rate=[&](double temp){return std::exp2((temp-options_.reference_c)/options_.half_life_degrees)/options_.retention_seconds;};
        exposure+=dt*(rate(stats_.temperature_c)+rate(temperature))*0.5;
        if(!std::isfinite(temperature) || !std::isfinite(exposure))throw std::overflow_error("environment model overflow");
    }
    stats_.temperature_c=temperature;stats_.peak_c=(std::max)(stats_.peak_c,temperature);
    stats_.exposure=exposure;stats_.media_energy_j+=energy;stats_.time_ps=t;++stats_.samples;previous_=b;
}
}
