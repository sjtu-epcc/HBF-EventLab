#include "hbf_eventlab/runtime/device_driver.hpp"
#include "hbf_eventlab/host/host_driver.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include <iostream>

using namespace hbf_eventlab;
int main() {
    try {
        Geometry geometry{1,1,1,8,4,1};
        StaticDeviceDescription description{geometry,{256,8,false,true},{256,Tick{10000000},geometry.block_group_dlus()}};
        EventQueue events;events.set_limits({100000,10000,10000});
        DeviceDriver device(description,{},events);
        RoundRobinInterleaver interleaver({},1,geometry.channel_bytes());
        HostDriver host(device,events,interleaver);
        host.set_completion_handler([&](const GlobalCompletion& completed) {
            std::cout<<"GLOBAL "<<completed.result.id.value<<" state="
                <<(completed.result.state==LogicalResult::Success?"Success":"Failed")
                <<" tick="<<completed.at.value<<'\n';
            if(completed.result.id==GlobalRequestId{2} && completed.result.state==LogicalResult::Success) {
                GlobalRequest read{GlobalRequestId{3},Op::Read,GlobalByteAddress{},4096};
                read.not_before=events.now();read.expected_content=1;host.submit(read);
            }
        });
        host.start_incremental();
        host.submit(GlobalRequest{GlobalRequestId{1},Op::Write,GlobalByteAddress{},2048,1});
        // Register the known producer arrival before running the queue. An external
        // producer must provide a safe time frontier, not race EventQueue::run().
        events.schedule_at(Tick{100000},EventPhase::Arrival,[&] {
            GlobalRequest rest{GlobalRequestId{2},Op::Write,GlobalByteAddress{2048},2048,1};
            rest.not_before=events.now();host.submit(rest);
        });
        events.run();
        if(host.results().size()!=3 || !host.workload_success() ||
           !host.activity().drained() || !device.activity().drained())return 1;
        std::cout<<"PASS incremental split write and completion-driven read; transient state drained\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
