#pragma once
#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include "online_audit.hpp"
#include <functional>
#include <chrono>

struct HbfDependencyError : std::runtime_error {using std::runtime_error::runtime_error;};
class S3Gate {
    using Bridge=hbf_eventlab::TensorAccessBridge;
    fs::path out_;
    json regions_,initialization_=json::array(),accesses_=json::array(),final_;
    OnlineAudit audit_;
    std::unique_ptr<Bridge> bridge_;
    std::ofstream gates_,tokens_;
    std::function<std::uint64_t()> stamp_;
    bool started_=false,finished_=false,init_failed_=false,failed_=false;
    std::uint64_t t0_=0,compute_=0,wait_=0,read_globals_=0;
    std::uint64_t initialization_wall_ns_=0,finish_wall_ns_=0;
    static std::uint64_t wall_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    static void check(bool ok,const char* why){if(!ok)throw HbfDependencyError(why);}
    json transfer(const json& r,bool init) {
        audit_.begin(init?hbf_eventlab::Op::Write:hbf_eventlab::Op::Read,r.at("global_base"),r.at("valid_bytes"));
        const auto result=init?bridge_->initialize(r.at("region_id")):bridge_->access(r.at("region_id"),0,r.at("valid_bytes"));
        if(!result.complete||!result.success||!result.completed) {
            failed_=true;init_failed_=init;throw HbfDependencyError("HBF transfer failed: "+result.reason);
        }
        audit_.end(result.registered_globals,result.finished_globals,result.completed->value);
        return {{"access_id",result.id},{"region_id",r.at("region_id")},{"tensor",r.at("tensor")},
            {"global_address",r.at("global_base")},{"bytes",r.at("valid_bytes")},
            {"offered_ps",result.offered.value},{"completed_ps",result.completed->value},
            {"registered_globals",result.registered_globals},{"finished_globals",result.finished_globals},
            {"capacity_rejections",result.capacity_rejections},{"complete",true},{"success",true}};
    }
public:
    S3Gate(const fs::path& out,const json& c,std::function<std::uint64_t()> stamp)
        :out_(out),regions_(c.at("regions")),gates_(out/"gates.jsonl"),tokens_(out/"tokens.jsonl"),stamp_(std::move(stamp)) {
        check(bool(gates_)&&bool(tokens_),"Cannot create gate outputs");
        std::ifstream f(c.at("effective_config_path").get<std::string>());check(bool(f),"Missing HBF config");
        auto config=hbf_eventlab::read_configuration(f,"FFN experiment");
        if(c.value("persistent_program_fail",false))config.faults.persistent_page=0;
        auto resolved=hbf_eventlab::resolve_configuration(config);
        check(resolved.values().device.geometry.host_channels==1,"Online audit requires one channel");
        std::ofstream eff(out/"effective.ini");hbf_eventlab::write_effective_configuration(eff,resolved);check(bool(eff),"Cannot export config");
        std::vector<hbf_eventlab::TensorRegion> registered;
        for(const auto& r:regions_) {
            check(r.at("source_offset")==0&&r.at("source_bytes")==r.at("valid_bytes")&&r.at("valid_bytes")==r.at("registered_bytes"),"Expected complete aligned FFN matrices");
            registered.push_back({r.at("region_id"),hbf_eventlab::GlobalByteAddress{r.at("global_base").get<std::uint64_t>()},r.at("registered_bytes"),1});
        }
        bridge_=std::make_unique<Bridge>(resolved,registered,hbf_eventlab::TensorBridgeConfiguration{},&audit_);
    }
    void initialize() {
        const auto begin=wall_ns();
        for(const auto& r:regions_)initialization_.push_back(transfer(r,true));
        initialization_wall_ns_=wall_ns()-begin;
    }
    void request_begin(){check(!started_,"Request already started");started_=true;t0_=bridge_->session().now().value;}
    json advance_segment(std::uint64_t duration) {
        check(started_&&!finished_,"Compute outside request");const auto before=bridge_->session().now().value;
        check(bridge_->advance_compute(hbf_eventlab::Tick{duration}).success,"Compute continuation failed");
        compute_=hbf_eventlab::checked_add(compute_,duration);
        check(bridge_->session().now().value==before+duration,"Compute clock mismatch");
        return {{"offered_ps",before},{"completed_ps",before+duration}};
    }
    void before(const json& node,int step) {
        check(started_&&!finished_,"Access outside request");
        for(const auto& cb:node.at("weight_accesses"))for(const auto& r:regions_) {
            if(cb.at("source").at("name")!=r.at("tensor"))continue;
            check(node.at("op")=="MUL_MAT"&&cb.at("source_offset")==0&&cb.at("source_bytes")==r.at("source_bytes"),"Unexpected selected consumer range/op");
            const auto before=stamp_();auto row=transfer(r,false);const auto after=stamp_();
            row.update({{"step",step},{"node_id",node.at("node_id")},{"callback_id",cb.at("callback_id")},{"before_ns",before},{"after_ns",after}});
            wait_+=row.at("completed_ps").get<std::uint64_t>()-row.at("offered_ps").get<std::uint64_t>();
            read_globals_+=row.at("registered_globals").get<std::uint64_t>();
            gates_<<row.dump()<<'\n';check(bool(gates_),"Gate output failed");accesses_.push_back(row);
        }
    }
    void token(int id,std::size_t index,bool eos) {
        const auto tick=bridge_->session().now().value;check(tick-t0_==compute_+wait_,"Token conservation failed");
        tokens_<<json({{"type",eos?"EOS":"token_emitted"},{"token_id",id},{"output_index",index},
            {"tick_ps",tick},{"request_t0_ps",t0_},{"relative_ps",tick-t0_},{"compute_ps",compute_},{"wait_ps",wait_}}).dump()<<'\n';
        check(bool(tokens_),"Token output failed");
    }
    const json& finish() {
        if(finished_)return final_;const auto begin=wall_ns();const auto r=bridge_->finish();finished_=true;finish_wall_ns_=wall_ns()-begin;
        const auto& h=r.run.host;const auto& d=r.run.device;const auto& s=bridge_->session().host().statistics();
        final_={{"status",hbf_eventlab::to_string(r.run.status)},{"workload_success",r.run.workload_success},
            {"input_closed",r.input_closed},{"host_drained",h.drained()},{"device_drained",d.drained()},
            {"explanation",r.run.explanation},{"events_processed",r.run.events},{"finished_ps",r.run.at.value},
            {"request_t0_ps",t0_},{"compute_ps",compute_},{"accumulated_wait_ps",wait_},{"read_globals",read_globals_},
            {"initialization_wall_ns",initialization_wall_ns_},{"finish_wall_ns",finish_wall_ns_},
            {"capacity_bytes",bridge_->session().configuration().values().device.geometry.channel_bytes()},
            {"dependency_failed",failed_},{"initialization",initialization_},{"accesses",accesses_},
            {"online_audit",{{"logical_failures",audit_.failures},{"attempt_errors",audit_.attempt_errors},
                {"read_fragments",audit_.total_read_fragments},{"write_fragments",audit_.total_write_fragments},
                {"peak_live_fragments",audit_.peak_work},{"peak_live_attempts",audit_.peak_attempts}}},
            {"host_logical_success",s.logical_success},{"host_logical_failed",s.logical_failed},{"host_attempt_errors",s.attempt_errors}};
        std::ofstream f(out_/"session.json");f<<final_.dump(2);check(bool(f),"Session output failed");gates_.flush();tokens_.flush();return final_;
    }
    std::uint64_t submitted() const{return read_globals_;}
    bool initialization_failed() const{return init_failed_;}
};
