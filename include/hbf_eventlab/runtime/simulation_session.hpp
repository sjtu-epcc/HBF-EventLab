#pragma once
#include "hbf_eventlab/configuration/simulation_configuration.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"

namespace hbf_eventlab {
struct SessionReport {
    RunReport run;
    bool input_closed;
    // Earliest external timestamp; false excludes the floor tick itself.
    Tick input_floor;
    bool accepts_at_floor;
    std::optional<Tick> next_event;
    InputCapacity input_capacity;
    bool feedback_yield=false; // Paused after an explicit callback handoff
};

// One logical external producer, all calls on the same simulation thread.
// Owns the existing Host/Device, not an alternative streaming Host. No wall-time
// waits, threads, sockets, or file I/O. The optional observer outlives this object.
class SimulationSession {
public:
    explicit SimulationSession(ResolvedSimulationConfiguration, IStatisticsSink* = nullptr,RuntimeProfile* = nullptr);
    SimulationSession(const SimulationSession&)=delete;
    SimulationSession& operator=(const SimulationSession&)=delete;
    void set_completion_handler(std::function<void(const GlobalCompletion&)>);
    void set_input_capacity_handler(std::function<void(InputCapacity)>);
    // External registration: outside advance/callbacks, before close_input.
    // Rejected batches have the same input-contract atomicity as Host::submit.
    void submit(const std::vector<GlobalRequest>&);
    void submit(const GlobalRequest& r) { submit(std::vector<GlobalRequest>{r}); }
    InputAdmission try_submit(const std::vector<GlobalRequest>&);
    InputAdmission try_submit(const GlobalRequest& r) { return try_submit(std::vector<GlobalRequest>{r}); }
    // Only within this session's completion/capacity callback or resumed causal
    // continuation, including after close.
    // It already runs as an event; Host release/pump obey same-tick causality.
    void submit_feedback(const std::vector<GlobalRequest>&);
    void submit_feedback(const GlobalRequest& r) { submit_feedback(std::vector<GlobalRequest>{r}); }
    InputAdmission try_submit_feedback(const std::vector<GlobalRequest>&);
    InputAdmission try_submit_feedback(const GlobalRequest& r) { return try_submit_feedback(std::vector<GlobalRequest>{r}); }
    InputCapacity input_capacity() const { return host_.input_capacity(); }
    void close_input(); // idempotent; does not cancel accepted work or feedback
    SessionReport advance_before(Tick); // exclusive endpoint, not a run limit
    // The producer promises no independent external input before this handoff.
    // With no event and open input, returns WaitingForInput without jumping time.
    SessionReport advance_until_yield();
    void request_yield(); // only in this session's feedback callback; never recursive
    // One-shot causal continuation of the last feedback yield, at the same Tick.
    // Does not reopen that Tick to ordinary external submit or undo deadlines.
    void resume_feedback(std::function<void()>);
    bool can_resume_feedback() const { return feedback_available_; }
    SessionReport run_to_completion(); // requires external input closed
    // Absolute safety ceilings, preserved across pauses; explicit changes only.
    void set_run_limits(RunLimits);
    const RunLimits& run_limits() const { return limits_; }
    Tick now() const { return events_.now(); }
    Tick input_floor() const { return events_.now(); }
    bool accepts_at_floor() const { return !events_.tick_started(); }
    bool input_closed() const { return input_closed_; }
    std::optional<Tick> next_event_time();
    const HostDriver& host() const { return host_; }
    const DeviceDriver& device() const { return device_; }
    const ResolvedSimulationConfiguration& configuration() const { return config_; }
private:
    void outside_execution() const;
    void healthy() const;
    void start();
    SessionReport drive(std::optional<Tick>);
    SessionReport report(RunStatus,const std::string&,bool feedback_yield=false);
    ResolvedSimulationConfiguration config_;
    EventQueue events_;
    DeviceDriver device_;
    RoundRobinInterleaver interleaver_;
    HostDriver host_;
    RunLimits limits_;
    bool input_closed_=false, started_=false;
    bool running_=false, in_feedback_=false;
    bool yield_requested_=false, feedback_available_=false;
    std::optional<std::pair<RunStatus,std::string>> failure_;
};
} // namespace hbf_eventlab
