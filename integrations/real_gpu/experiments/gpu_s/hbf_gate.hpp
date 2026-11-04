#pragma once
// Experimental application adapter; uses only the existing public bridge API.
#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <algorithm>
#include <functional>

struct HbfDependencyError : std::runtime_error { using std::runtime_error::runtime_error; };

class S3Gate {
    using Bridge = hbf_eventlab::TensorAccessBridge;
    using Result = hbf_eventlab::TensorAccessResult;
    fs::path out_;
    json regions_, initialization_=json::array(), access_results_=json::array();
    hbf_eventlab::StatisticsCollector stats_;
    std::unique_ptr<Bridge> bridge_;
    std::ofstream trace_, gates_, tokens_;
    std::function<std::uint64_t()> stamp_;
    bool started_=false, finished_=false, dependency_failed_=false;
    std::string failure_phase_;
    std::uint64_t active_node_=0, active_callback_=0, t0_=0, wait_=0, zero_calls_=0, read_globals_=0, compute_=0;
    json final_;
    static void require(bool okay,const char* why) { if(!okay)throw std::runtime_error(why); }
    static json result(const Result& r) {
        return {{"access_id",r.id},{"op",r.op==hbf_eventlab::Op::Read?"read":"write"},{"offered_ps",r.offered.value},
            {"completed_ps",r.completed?json(r.completed->value):json(nullptr)},
            {"complete",r.complete},{"success",r.success},{"registered_globals",r.registered_globals},
            {"finished_globals",r.finished_globals},{"capacity_rejections",r.capacity_rejections},
            {"unsubmitted_bytes",r.unsubmitted_bytes},{"stop",hbf_eventlab::to_string(r.stop)},{"reason",r.reason}};
    }
public:
    S3Gate(const fs::path& out,const json& config,std::function<std::uint64_t()> stamp)
        :out_(out),regions_(config.at("regions")),trace_(out/"bridge.jsonl"),gates_(out/"gates.jsonl"),
         tokens_(out/"tokens.jsonl"),stamp_(std::move(stamp)) {
        require(bool(trace_)&&bool(gates_)&&bool(tokens_),"Cannot create bridge records");
        std::ifstream ini(config.at("effective_config_path").get<std::string>());
        require(bool(ini),"Cannot read frozen H0 configuration");
        auto c=hbf_eventlab::read_configuration(ini,"S0 frozen H0");
        if(config.value("persistent_program_fail",false))c.faults.persistent_page=0;
        if(config.contains("max_tick_ps"))c.run.max_tick=hbf_eventlab::Tick{config.at("max_tick_ps").get<std::uint64_t>()};
        auto resolved=hbf_eventlab::resolve_configuration(c);
        std::ofstream effective(out/"effective.ini");hbf_eventlab::write_effective_configuration(effective,resolved);
        std::vector<hbf_eventlab::TensorRegion> registered;
        for(const auto& r:regions_) {
            const auto offset=r.at("source_offset").get<std::uint64_t>(),bytes=r.at("valid_bytes").get<std::uint64_t>();
            require(offset%64==0&&bytes>0&&bytes%64==0&&offset<=r.at("source_bytes").get<std::uint64_t>()&&
                    bytes<=r.at("source_bytes").get<std::uint64_t>()-offset,"Invalid source registration");
            require(r.at("registered_bytes").get<std::uint64_t>()==(bytes+4095)/4096*4096,"Invalid padded registration");
            registered.push_back({r.at("region_id"),hbf_eventlab::GlobalByteAddress{r.at("global_base").get<std::uint64_t>()},
                                  r.at("registered_bytes"),r.at("content_id")});
        }
        bridge_=std::make_unique<Bridge>(resolved,std::move(registered),hbf_eventlab::TensorBridgeConfiguration{},&stats_);
        bridge_->set_trace([this](const hbf_eventlab::TensorBridgeTrace& e) {
            trace_<<json({{"action",e.action},{"access_id",e.access},{"global_id",e.global.value},{"tick_ps",e.at.value},
                          {"node_id",active_node_},{"callback_id",active_callback_}}).dump()<<'\n';
            require(bool(trace_),"Bridge trace write failed");
        });
    }
    void initialize() {
        for(const auto& region:regions_) {
            const auto r=bridge_->initialize(region.at("region_id"));
            auto row=result(r);row["region_id"]=region.at("region_id");initialization_.push_back(row);
            if(!r.complete||!r.success||!r.completed) {
                dependency_failed_=true;failure_phase_="initialization";
                throw HbfDependencyError("HBF initialization did not succeed: "+r.reason);
            }
        }
    }
    void request_begin() { started_=true;t0_=bridge_->session().now().value; }
    json advance_segment(std::uint64_t duration) {
        require(started_&&!finished_,"Segment outside active request");
        const auto before=bridge_->session().now().value;
        const auto r=bridge_->advance_compute(hbf_eventlab::Tick{duration});
        if(!r.success)throw HbfDependencyError("Segment compute continuation failed");
        compute_+=duration;
        require(bridge_->session().now().value==before+duration,"Segment advance mismatch");
        return {{"offered_ps",before},{"completed_ps",before+duration}};
    }
    void before(const json& node,int step) {
        require(started_&&!finished_,"HBF request not active");
        const auto compute=bridge_->advance_compute(hbf_eventlab::Tick{0});++zero_calls_;
        if(!compute.success)throw HbfDependencyError("Zero compute continuation failed");
        for(const auto& callback:node.at("weight_accesses"))for(const auto& region:regions_) {
            if(callback.at("source").at("name")!=region.at("tensor"))continue;
            const auto actual_bytes=callback.at("source_bytes").get<std::uint64_t>();
            require(actual_bytes==region.at("source_bytes").get<std::uint64_t>(),"Actual tensor bytes differ from manifest");
            const auto source=callback.at("source_offset").get<std::uint64_t>();
            const auto origin=region.at("source_offset").get<std::uint64_t>();
            const auto begin=std::max(source,origin),end=std::min(source+actual_bytes,origin+region.at("valid_bytes").get<std::uint64_t>());
            if(begin>=end)continue;
            active_node_=node.at("node_id");active_callback_=callback.at("callback_id");
            const auto before_ns=stamp_();const auto r=bridge_->access(region.at("region_id"),begin-origin,end-begin);
            const auto after_ns=stamp_();auto row=result(r);
            row.update({{"node_id",active_node_},{"callback_id",active_callback_},{"step",step},{"region_id",region.at("region_id")},
                        {"tensor",region.at("tensor")},{"source_offset",begin},{"region_offset",begin-origin},
                        {"global_address",region.at("global_base").get<std::uint64_t>()+begin-origin},{"bytes",end-begin},
                        {"before_ns",before_ns},{"after_ns",after_ns},{"content_id",region.at("content_id")}});
            gates_<<row.dump()<<'\n';require(bool(gates_),"Gate record write failed");access_results_.push_back(row);
            read_globals_+=r.registered_globals;
            if(!r.complete||!r.success||!r.completed) {
                dependency_failed_=true;failure_phase_="consumer_dependency";
                throw HbfDependencyError("HBF consumer dependency did not succeed: "+r.reason);
            }
            wait_+=r.completed->value-r.offered.value;active_node_=active_callback_=0;
        }
    }
    void token(int id,std::size_t index,bool eos) {
        require(bridge_->advance_compute(hbf_eventlab::Tick{0}).success,"Token tail continuation failed");++zero_calls_;
        const auto tick=bridge_->session().now().value;
        require(tick-t0_==wait_+compute_,"Token clock does not equal accumulated compute and HBF wait");
        tokens_<<json({{"type",eos?"EOS":"token_emitted"},{"token_id",id},{"output_index",index},
                       {"tick_ps",tick},{"request_t0_ps",t0_},{"relative_ps",tick-t0_},{"compute_ps",compute_},{"wait_ps",wait_}}).dump()<<'\n';
        require(bool(tokens_),"Token record write failed");
    }
    const json& finish() {
        if(finished_)return final_;
        const auto r=bridge_->finish();finished_=true;
        const auto& h=r.run.host;const auto& d=r.run.device;
        final_={{"status",hbf_eventlab::to_string(r.run.status)},{"workload_success",r.run.workload_success},
            {"input_closed",r.input_closed},{"host_drained",h.drained()},{"device_drained",d.drained()},
            {"finished_ps",r.run.at.value},{"next_event_ps",r.next_event?json(r.next_event->value):json(nullptr)},
            {"explanation",r.run.explanation},{"events_processed",r.run.events},
            {"host_unfinished_globals",h.unfinished_global},{"host_queued_units",h.queued_units},{"host_unsettled_attempts",h.unsettled_attempts},
            {"request_started",started_},{"request_t0_ps",started_?json(t0_):json(nullptr)},{"compute_ps",compute_},
            {"accumulated_wait_ps",wait_},{"zero_compute_calls",zero_calls_},{"read_globals",read_globals_},
            {"dependency_failed",dependency_failed_},{"failure_phase",failure_phase_},
            {"failed_consumer_node_id",dependency_failed_&&active_node_?json(active_node_):json(nullptr)},
            {"initialization",initialization_},{"accesses",access_results_},{"regions",regions_}};
        std::ofstream(out_/"session.json")<<final_.dump(2);
        std::ofstream statistics(out_/"statistics.json");stats_.write_json(statistics,bridge_->session().now(),hbf_eventlab::to_string(r.run.status));
        require(bool(statistics),"Statistics record write failed");trace_.flush();gates_.flush();tokens_.flush();return final_;
    }
    std::uint64_t submitted() const { return read_globals_; }
    bool initialization_failed() const { return failure_phase_=="initialization"; }
};
