#include "hbf_eventlab/runtime/simulation_session.hpp"

namespace hbf_eventlab {
namespace {
struct FlagScope {
    bool& flag;
    explicit FlagScope(bool& value):flag(value) { flag=true; }
    ~FlagScope() { flag=false; }
};
}
SimulationSession::SimulationSession(ResolvedSimulationConfiguration config,IStatisticsSink* sink)
    :config_(std::move(config)),device_(config_.description(),config_.values().execution,events_),
     interleaver_(config_.values().address.global_base,config_.description().geometry().host_channels,
                  config_.description().geometry().channel_bytes(),config_.values().address.interleave_bytes),
     host_(device_,events_,interleaver_,config_.values().host),limits_(config_.values().run) {
    events_.set_limits(limits_.events);
    device_.set_program_outcome(make_program_outcome(config_.values().faults));
    if(sink) { device_.set_statistics_sink(*sink);host_.set_statistics_sink(*sink); }
}
void SimulationSession::outside_execution() const {
    if(running_)throw std::logic_error("session operation is external-only and not reentrant");
}
void SimulationSession::healthy() const {
    if(failure_)throw std::logic_error("session has a terminal model/implementation error");
}
void SimulationSession::start() {
    if(!started_) { host_.start_incremental();started_=true; }
}
void SimulationSession::set_completion_handler(std::function<void(const GlobalCompletion&)> handler) {
    outside_execution();healthy();
    if(started_ || input_closed_ || !handler)throw std::logic_error("attach a nonempty completion handler before session input/advance/close");
    host_.set_completion_handler([this,handler=std::move(handler)](const GlobalCompletion& result) {
        FlagScope feedback(in_feedback_);
        handler(result);
    });
}
void SimulationSession::submit(const std::vector<GlobalRequest>& input) {
    outside_execution();healthy();
    if(input_closed_)throw std::logic_error("external input is closed");
    for(const auto& r:input)
        if(r.not_before<input_floor() || (!accepts_at_floor() && r.not_before==input_floor()))
            throw std::invalid_argument("external request is before the open input boundary or in an already entered tick");
    if(!started_) { host_.start_incremental(input);started_=true; }
    else host_.submit(input);
}
void SimulationSession::submit_feedback(const std::vector<GlobalRequest>& input) {
    healthy();
    if(!running_ || !in_feedback_)throw std::logic_error("feedback submission requires this session's completion event");
    // This is already an asynchronous Host event. It may create a new attempt at
    // the current Tick, but cannot enter the frozen batch that generated it.
    host_.submit(input);
}
void SimulationSession::close_input() {
    outside_execution();healthy();input_closed_=true;
}
void SimulationSession::set_run_limits(RunLimits limits) {
    outside_execution();healthy();
    if(limits.max_tick && *limits.max_tick<events_.now())throw std::invalid_argument("run time ceiling is before the current clock");
    limits_=limits;events_.set_limits(limits_.events); // counters are never reset
}
std::optional<Tick> SimulationSession::next_event_time() {
    outside_execution();return events_.next_time();
}
SessionReport SimulationSession::report(RunStatus status,const std::string& why) {
    return {{status,host_.workload_success(),events_.now(),events_.executed(),host_.activity(),device_.activity(),why},
            input_closed_,input_floor(),accepts_at_floor(),events_.next_time()};
}
SessionReport SimulationSession::advance_before(Tick before) {
    outside_execution();
    if(before<events_.now() || (before==input_floor() && !accepts_at_floor()))
        throw std::invalid_argument("exclusive pause boundary is already entered or in the past");
    return drive(before);
}
SessionReport SimulationSession::run_to_completion() {
    outside_execution();
    if(!input_closed_)throw std::logic_error("run_to_completion requires closed external input; use advance_before");
    return drive(std::nullopt);
}
SessionReport SimulationSession::drive(std::optional<Tick> before) {
    if(failure_)return report(failure_->first,failure_->second);
    FlagScope execution(running_);
    try {
        start();
        for(;;) {
            const auto next=events_.next_time();
            if(!next && input_closed_) {
                if(host_.activity().drained() && device_.activity().drained())
                    return report(RunStatus::Completed,host_.workload_success()?"All logical work succeeded":"Logical failures recorded; transient work drained");
                return report(RunStatus::Stalled,host_.waiting_reason()+"; input closed with unfinished work and no live event");
            }
            // A normal pause makes a promise about the entire empty interval,
            // not just the timestamp of the last hardware event.
            if(!next || (before && !(*next<*before))) {
                const Tick endpoint=*before; // unbounded drive requires closed input
                if(limits_.max_tick && *limits_.max_tick<endpoint) {
                    events_.advance_idle_to(*limits_.max_tick);
                    return report(RunStatus::LimitReached,"Requested horizon exceeds simulated-time ceiling; state retained");
                }
                events_.advance_idle_to(endpoint);
                return report(next?RunStatus::Paused:RunStatus::WaitingForInput,
                    next?"Exclusive pause boundary reached":"No live event; external input remains open");
            }
            if(limits_.max_tick && *limits_.max_tick<*next) {
                events_.advance_idle_to(*limits_.max_tick);
                return report(RunStatus::LimitReached,"Next event exceeds simulated-time ceiling; state retained");
            }
            events_.run_next(); // normal rounds and indivisible deadline settlement
        }
    } catch(const EventLimitReached& e) { return report(RunStatus::LimitReached,e.what()); }
      catch(const std::invalid_argument& e) { failure_={{RunStatus::ModelError,e.what()}}; }
      catch(const std::exception& e) { failure_={{RunStatus::ImplementationError,e.what()}}; }
      catch(...) { failure_={{RunStatus::ImplementationError,"Nonstandard exception from session callback"}}; }
    return report(failure_->first,failure_->second);
}
} // namespace hbf_eventlab
