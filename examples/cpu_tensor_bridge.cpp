#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include <iostream>
#include <numeric>

using namespace hbf_eventlab;
int main() {
    try {
        SimulationConfiguration config;
        config.device.geometry.host_channels=1;
        config.host.max_resident_fragments=64;
        TensorAccessBridge bridge(resolve_configuration(config),{{"weights",GlobalByteAddress{},8192,1}});
        bridge.set_trace([](const TensorBridgeTrace& e) {
            std::cout<<"BRIDGE "<<e.action<<" access="<<e.access<<" global="<<e.global.value<<" tick="<<e.at.value<<'\n';
        });
        // The executor owns these real numbers. HBF never receives their bytes.
        std::vector<float> weights(8192/sizeof(float),2.0f);
        const auto initialized=bridge.initialize("weights");
        if(!initialized.success)throw std::runtime_error(initialized.reason);
        std::size_t consumed=0;
        auto gate=[&](std::uint64_t offset,std::uint64_t bytes,Tick compute) {
            const auto r=bridge.access("weights",offset,bytes,compute);
            if(!r.success)throw std::runtime_error(r.reason); // no executor continuation on failure
            ++consumed;
            std::cout<<"ACCESS offered="<<r.offered.value<<" completed="<<r.completed->value
                     <<" globals="<<r.finished_globals<<" input_backpressure="<<r.capacity_rejections<<'\n';
        };
        gate(0,8192,Tick{250000});
        const auto sum=std::accumulate(weights.begin(),weights.end(),0.0f);
        gate(0,64,Tick{}); // same-Tick causal continuation, without a fabricated Tick
        const auto done=bridge.finish();
        const bool okay=done.run.status==RunStatus::Completed&&done.run.workload_success&&sum==4096.0f&&consumed==2;
        std::cout<<"CPU_BRIDGE status="<<to_string(done.run.status)<<" success="<<okay
                 <<" callbacks="<<consumed<<" cpu_sum="<<sum<<" simulated_ticks="<<done.run.at.value
                 <<" host_drained="<<done.run.host.drained()<<" device_drained="<<done.run.device.drained()<<'\n';
        return okay?0:1;
    } catch(const std::exception& e) {std::cerr<<"CPU_BRIDGE failure: "<<e.what()<<'\n';return 1;}
}
