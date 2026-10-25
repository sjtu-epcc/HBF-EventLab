#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "hbf_eventlab/statistics/event_stream.hpp"
#include <fstream>
#include <iostream>
using namespace hbf_eventlab;
int main(int argc,char** argv) {
    try {
        std::ofstream file;
        std::unique_ptr<StatisticsEventStream> stream;
        if(argc>2)throw std::invalid_argument("usage: hbf_eventlab_bounded_demo [events.jsonl]");
        if(argc==2){file.open(argv[1],std::ios::binary);stream=std::make_unique<StatisticsEventStream>(file,4096);}
        SimulationConfiguration c;c.device.geometry.host_channels=1;
        c.host.max_resident_fragments=64;c.host.max_logical_fragments=0;
        SimulationSession session(resolve_configuration(c),stream.get());
        session.set_completion_handler([&](const GlobalCompletion& e){
            std::cout<<"GLOBAL "<<e.result.id.value<<" success="<<(e.result.state==LogicalResult::Success)
                     <<" tick="<<e.at.value<<" resident="<<session.input_capacity().resident<<'\n';
        });
        session.submit(GlobalRequest{GlobalRequestId{1},Op::Write,GlobalByteAddress{},4096,1});
        GlobalRequest next{GlobalRequestId{2},Op::Read,GlobalByteAddress{},4096};
        next.expected_content=1;next.after={GlobalRequestId{1}};next.offered_at=session.now();
        unsigned blocked=0;
        for(;;) {
            const auto admission=session.try_submit(next);
            if(admission.disposition==InputDisposition::Registered)break;
            if(admission.disposition==InputDisposition::TooLarge)
                throw std::runtime_error("producer batch exceeds total input budget");
            ++blocked;
            // This example producer promises no other external inputs in this
            // interval. A GPU adapter must provide its own safe horizon.
            auto r=session.advance_before(Tick{checked_add(session.now().value,500000)});
            if(r.run.status!=RunStatus::Paused&&r.run.status!=RunStatus::WaitingForInput)
                throw std::runtime_error(r.run.explanation);
            next.not_before=session.now(); // offered_at remains the original time
        }
        session.close_input();const auto report=session.run_to_completion();
        if(stream)stream->finish(session.now(),to_string(report.run.status));
        const auto storage=session.host().storage();
        std::cout<<"STOP "<<to_string(report.run.status)<<" input_retries="<<blocked
                 <<" work="<<storage.work<<" units="<<storage.units<<" retained="<<storage.retained
                 <<" result_summaries="<<storage.results<<'\n';
        return report.run.status==RunStatus::Completed&&report.run.workload_success&&blocked&&
               storage.results==2&&!storage.work&&!storage.units?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
