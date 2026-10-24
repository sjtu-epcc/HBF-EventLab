#include "hbf_eventlab/runtime/simulation_runner.hpp"
namespace hbf_eventlab {
const char* to_string(RunStatus s) {
    switch(s) {
    case RunStatus::Completed:return "Completed";case RunStatus::Stalled:return "Stalled";
    case RunStatus::LimitReached:return "LimitReached";case RunStatus::ModelError:return "ModelError";
    case RunStatus::Paused:return "Paused";case RunStatus::WaitingForInput:return "WaitingForInput";
    default:return "ImplementationError";
    }
}
RunReport SimulationRunner::report(RunStatus s,const std::string& why) {
    return {s,host_.workload_success(),events_.now(),events_.executed(),host_.activity(),device_(),why};
}
RunReport SimulationRunner::run(RunLimits limits) {
    // This runner's Completed/Stalled contract describes a finite, preloaded input.
    // Incremental callers currently drive the shared queue at explicit boundaries.
    if(host_.incremental_input())return report(RunStatus::ModelError,
        "finite SimulationRunner does not support open incremental input; drive EventQueue explicitly");
    events_.set_limits(limits.events);
    try {
        for(;;) {
            const auto next=events_.next_time(); // excludes canceled heap tombstones
            if(!next) {
                if(host_.activity().drained()&&device_().drained())
                    return report(RunStatus::Completed,host_.workload_success()?"All logical work succeeded":"Logical failures recorded; transient work drained");
                return report(RunStatus::Stalled,host_.waiting_reason()+"; unfinished work with no live progress event");
            }
            if(limits.max_tick && *limits.max_tick<*next)return report(RunStatus::LimitReached,"Next event exceeds simulated-time limit; state retained");
            // Future timeout/window/operation completions are real progress events.
            // Jump directly to them even when no command is currently executable.
            events_.run_next();
        }
    } catch(const EventLimitReached& e) {return report(RunStatus::LimitReached,e.what());}
      catch(const std::invalid_argument& e) {return report(RunStatus::ModelError,e.what());}
      catch(const std::exception& e) {return report(RunStatus::ImplementationError,e.what());}
}
}

