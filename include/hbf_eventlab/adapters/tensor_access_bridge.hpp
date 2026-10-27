#pragma once
#include "hbf_eventlab/runtime/simulation_session.hpp"
#include <set>
#include <memory>

namespace hbf_eventlab {
// Immutable external address registration. Real bytes remain with the executor.
struct TensorRegion {
    std::string name;
    GlobalByteAddress base;
    std::uint64_t bytes,content;
};
struct TensorBridgeConfiguration {
    std::uint64_t global_chunk_bytes=4096; // Global grouping, not a Device command window
};
struct TensorAccessResult {
    std::uint64_t id=0;
    Op op=Op::Read;
    Tick offered;
    std::optional<Tick> completed;
    bool complete=false,success=false;
    std::uint64_t registered_globals=0,finished_globals=0,capacity_rejections=0,unsubmitted_bytes=0;
    RunStatus stop=RunStatus::Paused;
    std::string reason;
};
struct TensorBridgeTrace {
    const char* action;
    std::uint64_t access;
    GlobalRequestId global;
    Tick at;
};
struct TensorComputeResult {
    Tick started,at;
    bool success=false;
    RunStatus stop=RunStatus::Paused;
    std::string reason;
};
// A synchronous producer adapter, not a second Host. Single simulation thread;
// no CUDA, real payload, wall clock, sleeps, Device submission or NAND inspection.
class TensorAccessBridge {
public:
    TensorAccessBridge(ResolvedSimulationConfiguration,std::vector<TensorRegion>,
                       TensorBridgeConfiguration={},IStatisticsSink* = nullptr,RuntimeProfile* = nullptr);
    TensorAccessResult initialize(const std::string&); // writes the declared range once
    TensorAccessResult access(const std::string&,std::uint64_t offset,std::uint64_t bytes,Tick compute={});
    // Compute-only interval, including the tail after the final memory callback.
    // Zero preserves the same-Tick feedback continuation; no dummy read or Tick.
    TensorComputeResult advance_compute(Tick);
    SessionReport finish(); // abandon unregistered remainder, drain registered work
    void set_trace(std::function<void(const TensorBridgeTrace&)>);
    const SimulationSession& session() const {return session_;}
private:
    struct Active {
        TensorAccessResult result;
        GlobalByteAddress base;
        std::uint64_t bytes,content,next=0;
        bool failed=false,input_open=false;
        std::optional<std::uint64_t> waiting_fragments;
        std::set<GlobalRequestId> outstanding;
    };
    TensorAccessResult transfer(const TensorRegion&,Op,std::uint64_t,std::uint64_t,Tick);
    void pump(bool feedback);
    void completed(const GlobalCompletion&);
    void check_complete(bool feedback=true);
    void emit(const char*,GlobalRequestId={});
    SimulationSession session_;
    TensorBridgeConfiguration config_;
    std::map<std::string,TensorRegion> tensors_;
    std::set<std::string> initialized_;
    std::unique_ptr<Active> active_;
    std::function<void(const TensorBridgeTrace&)> trace_;
    std::uint64_t next_global_=0,next_access_=0;
    bool executing_=false,stopped_=false,closed_=false;
};
} // namespace hbf_eventlab
