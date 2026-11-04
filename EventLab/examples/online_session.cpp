#include "hbf_eventlab/runtime/simulation_session.hpp"
#include <iostream>

using namespace hbf_eventlab;
int main() {
    try {
        SimulationConfiguration config;
        config.device.geometry.host_channels=1;
        SimulationSession session(resolve_configuration(config));
        session.set_completion_handler([&](const GlobalCompletion& completed) {
            std::cout<<"GLOBAL "<<completed.result.id.value<<" tick="<<completed.at.value
                     <<" success="<<(completed.result.state==LogicalResult::Success)<<'\n';
            if(completed.result.id==GlobalRequestId{2} && completed.result.state==LogicalResult::Success) {
                GlobalRequest read{GlobalRequestId{3},Op::Read,GlobalByteAddress{},4096};
                read.not_before=session.now();read.expected_content=1;
                session.submit_feedback(read); // valid even after external input closes
            }
        });
        session.submit(GlobalRequest{GlobalRequestId{1},Op::Write,GlobalByteAddress{},2048,1});
        const auto paused=session.advance_before(Tick{100000});
        std::cout<<"STOP "<<to_string(paused.run.status)<<" tick="<<paused.run.at.value<<'\n';
        // The producer promises that no more external requests precede 100000.
        // The endpoint is still open; no wall-clock wait or extra Tick is added.
        GlobalRequest rest{GlobalRequestId{2},Op::Write,GlobalByteAddress{2048},2048,1};
        rest.not_before=session.now();session.submit(rest);
        session.close_input();
        const auto done=session.run_to_completion();
        std::cout<<"STOP "<<to_string(done.run.status)<<" tick="<<done.run.at.value
                 <<" events="<<done.run.events<<" input_closed="<<done.input_closed
                 <<" host_drained="<<done.run.host.drained()<<" device_drained="<<done.run.device.drained()<<'\n';
        if(done.run.status!=RunStatus::Completed || !done.run.workload_success || session.host().results().size()!=3)return 1;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
