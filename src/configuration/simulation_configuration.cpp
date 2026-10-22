#include "hbf_eventlab/configuration/simulation_configuration.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include "hbf_eventlab/policies/host_send_policy.hpp"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

namespace hbf_eventlab {
namespace {
std::string trim(const std::string& s) {
    const auto first=s.find_first_not_of(" \t\r\n");
    return first==std::string::npos?"":s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
std::string unquote(const std::string& input) {
    const auto s=trim(input);
    if(s.empty() || s.front()!='"') return s;
    std::istringstream in(s);std::string value;in>>std::quoted(value);
    if(!in || !(in>>std::ws).eof()) throw std::invalid_argument("malformed quoted value");
    return value;
}
std::uint64_t integer(const std::string& s) {
    if(s.empty() || s.front()=='-' || s.front()=='+') throw std::invalid_argument("expected unsigned integer");
    const bool hex=s.size()>2 && s[0]=='0' && (s[1]=='x'||s[1]=='X');
    const auto first=s.data()+(hex?2:0);std::uint64_t value=0;
    const auto result=std::from_chars(first,s.data()+s.size(),value,hex?16:10);
    if(result.ec!=std::errc{} || result.ptr!=s.data()+s.size()) throw std::invalid_argument("invalid or overflowing unsigned integer");
    return value;
}
template<class T> void parse(const std::string& s,T& value) {
    static_assert(std::is_unsigned_v<T> && !std::is_same_v<T,bool>);
    const auto n=integer(s);
    if(n>std::numeric_limits<T>::max()) throw std::invalid_argument("integer exceeds field range");
    value=static_cast<T>(n);
}
void parse(const std::string& s,bool& value) {
    if(s=="true")value=true;else if(s=="false")value=false;
    else throw std::invalid_argument("expected true or false");
}
void parse(const std::string& s,std::string& value) {
    if(s.find_first_of("\r\n")!=std::string::npos || s.find('\0')!=std::string::npos)
        throw std::invalid_argument("multiline/NUL strings are unsupported");
    value=s;
}
void parse(const std::string& s,Tick& value) { value=Tick{integer(s)}; }
void parse(const std::string& s,GlobalByteAddress& value) { value=GlobalByteAddress{integer(s)}; }
void parse(const std::string& s,UcieSpeedGrade& value) {
    std::uint32_t n=0;parse(s,n);value=static_cast<UcieSpeedGrade>(n);(void)ucie_speed_profile(value);
}
void parse(const std::string& s,WriteCommandMode& value) {
    if(s=="fragment64")value=WriteCommandMode::Fragment64;else if(s=="burst4096")value=WriteCommandMode::Burst4096;
    else throw std::invalid_argument("expected fragment64 or burst4096");
}
void parse(const std::string& s,ReadHintMode& value) {
    if(s=="tail_batch")value=ReadHintMode::TailBatch;else if(s=="all_regular")value=ReadHintMode::AllRegular;
    else throw std::invalid_argument("expected tail_batch or all_regular");
}
template<class T> void parse(const std::string& s,std::optional<T>& value) {
    if(s=="none")value.reset();else {T result{};parse(s,result);value=result;}
}
template<class T> void parse(const std::string& s,std::vector<T>& value) {
    if(s.size()<2 || s.front()!='[' || s.back()!=']') throw std::invalid_argument("expected [comma,separated,integers]");
    const auto inner=trim(s.substr(1,s.size()-2));std::vector<T> result;
    if(!inner.empty()) {
        if(inner.back()==',') throw std::invalid_argument("empty list element");
        std::istringstream in(inner);std::string part;
        while(std::getline(in,part,',')) {T item{};parse(trim(part),item);result.push_back(item);}
    }
    value=std::move(result);
}
template<class T> std::string format(T value) {return std::to_string(value);}
std::string format(bool value) {return value?"true":"false";}
std::string format(const std::string& value) {std::ostringstream out;out<<std::quoted(value);return out.str();}
std::string format(Tick value) {return std::to_string(value.value);}
std::string format(GlobalByteAddress value) {return std::to_string(value.value);}
std::string format(UcieSpeedGrade value) {return std::to_string(static_cast<std::uint32_t>(value));}
std::string format(WriteCommandMode value) {return value==WriteCommandMode::Fragment64?"fragment64":"burst4096";}
std::string format(ReadHintMode value) {return value==ReadHintMode::TailBatch?"tail_batch":"all_regular";}
template<class T> std::string format(const std::optional<T>& value) {return value?format(*value):"none";}
template<class T> std::string format(const std::vector<T>& value) {
    std::string result="[";for(const auto& v:value) {if(result.size()>1)result+=", ";result+=format(v);}return result+"]";
}
struct Field {std::function<void(const std::string&)> set;std::function<std::string()> get;};
using Fields=std::map<std::string,Field>;
// Bind only configuration data, never runtime state. The same field list drives
// parsing and effective export, so a newly exposed key cannot silently disappear.
Fields fields(SimulationConfiguration& c) {
    Fields out;
    auto add=[&](const std::string& key,auto& value) {
        out.emplace(key,Field{[&value](const auto& s){parse(s,value);},[&value]{return format(value);}});
    };
    auto& d=c.device;auto& e=c.execution;auto& h=c.host;
    add("device.spec_profile",d.spec_profile);add("device.ucie_speed_grade",e.ucie.speed_grade);
    add("device.geometry.host_channels",d.geometry.host_channels);
    add("device.geometry.dies_per_channel",d.geometry.dies_per_channel);
    add("device.geometry.banks_per_die",d.geometry.banks_per_die);
    add("device.geometry.pages_per_block",d.geometry.pages_per_block);
    add("device.geometry.blocks_per_bank",d.geometry.blocks_per_bank);
    add("device.geometry.spare_blocks_per_bank",d.geometry.spare_blocks_per_bank);
    add("device.capabilities.mocs_supported",d.capabilities.mocs_supported);
    add("device.capabilities.max_pending_write_dlus",d.capabilities.max_pending_write_dlus);
    add("device.capabilities.read_burst",d.capabilities.read_burst);add("device.capabilities.write_burst",d.capabilities.write_burst);
    add("device.selected.mocs_selected",d.selected.mocs_selected);
    add("device.selected.assembly_timeout_ps",d.selected.assembly_timeout);
    out.emplace("device.selected.zone_size_dlus",Field{[&d](const auto& s){
        if(s=="auto") d.selected.zone_size_dlus=0;
        else {const auto n=integer(s);if(!n)throw std::invalid_argument("use auto or a positive DLU count");d.selected.zone_size_dlus=n;}
    },[&d]{return d.selected.zone_size_dlus?format(d.selected.zone_size_dlus):"auto";}});
    add("device.topology.core_channels_per_ucie",d.topology.core_channels_per_ucie);
    add("device.topology.die_to_core_channel",d.topology.die_to_core_channel);
    add("device.capacity_policy.max_hole_permille",d.capacity_policy.max_hole_permille);
    add("address.global_base",c.address.global_base);add("address.interleave_bytes",c.address.interleave_bytes);
    add("host.send_policy",h.send_policy);add("host.write_mode",h.writes);add("host.read_hint_mode",h.hints);
    add("host.max_open_dlus_per_channel",h.max_open_dlus_per_channel);
    add("host.retained_fragments",h.retained_fragments);add("host.max_logical_fragments",h.max_logical_fragments);
    add("host.recovery.max_replay_passes",h.max_replay_passes);
    add("host.retry.max_pending_retries",h.max_pending_retries);add("host.retry.max_timeout_retries",h.max_timeout_retries);
    add("host.retry.pending_delay_ps",h.pending_retry_delay);
    add("execution.media_timing.sense_ps",e.media_timing.sense);add("execution.media_timing.program_ps",e.media_timing.program);
    add("execution.media_timing.erase_ps",e.media_timing.erase);add("execution.media_timing.cache_access_ps",e.media_timing.cache_access);
    add("execution.ucie.request_control_bytes",e.ucie.overhead.request_control_bytes);
    add("execution.ucie.response_control_bytes",e.ucie.overhead.response_control_bytes);
    add("execution.ucie.request_fixed_latency_ps",e.ucie.overhead.request_fixed_latency);
    add("execution.ucie.response_fixed_latency_ps",e.ucie.overhead.response_fixed_latency);
    add("execution.buffers.ingress_entries",e.buffers.ingress.entries);add("execution.buffers.ingress_payload_bytes",e.buffers.ingress.payload_bytes);
    add("execution.buffers.egress_entries",e.buffers.egress.entries);add("execution.buffers.egress_payload_bytes",e.buffers.egress.payload_bytes);
    add("execution.read_dispatch.policy",e.read_dispatch.policy);add("execution.read_dispatch.wait_ps",e.read_dispatch.wait);
    add("execution.read_dispatch.threshold",e.read_dispatch.threshold);
    add("execution.nand_scheduler.policy",e.nand_scheduler);add("execution.ucie_scheduler.policy",e.ucie_scheduler);
    add("faults.channel",c.faults.target.channel);add("faults.die",c.faults.target.die);
    add("faults.bank",c.faults.target.bank);add("faults.block",c.faults.target.block);
    add("faults.fail_sequence",c.faults.fail_sequence);add("faults.persistent_page",c.faults.persistent_page);
    add("run.max_tick_ps",c.run.max_tick);add("run.total_events",c.run.events.total_events);
    add("run.events_per_tick",c.run.events.events_per_tick);add("run.rounds_per_tick",c.run.events.rounds_per_tick);
    add("output.trace",c.output.trace);add("output.statistics_json",c.output.statistics_json);add("output.effective_config",c.output.effective_config);
    return out;
}
void assign(Fields& available,const std::string& key,const std::string& value) {
    const auto it=available.find(key);
    if(it==available.end()) throw std::invalid_argument(key+": unknown or derived-only configuration field");
    try {it->second.set(unquote(value));}
    catch(const std::exception& e) {throw std::invalid_argument(key+"="+value+": "+e.what());}
}
void policy(const std::string& key,const std::string& value,const char* supported) {
    if(value!=supported)throw std::invalid_argument(key+": unsupported policy '"+value+"'; supported: "+supported);
}
}
SimulationConfiguration read_configuration(std::istream& in,const std::string& source) {
    SimulationConfiguration c;auto available=fields(c);std::set<std::string> assigned;
    std::string section,line;std::size_t number=0;
    while(std::getline(in,line)) {
        ++number;
        if(number==1 && line.compare(0,3,"\xEF\xBB\xBF")==0)line.erase(0,3);
        line=trim(line);if(line.empty() || line[0]=='#' || line[0]==';')continue;
        try {
            if(line[0]=='[') {
                if(line.back()!=']')throw std::invalid_argument("malformed section");
                section=trim(line.substr(1,line.size()-2));
                if(section.empty() || std::none_of(available.begin(),available.end(),[&](const auto& f){return f.first.rfind(section+".",0)==0;}))
                    throw std::invalid_argument("unknown section: "+section);
                continue;
            }
            const auto eq=line.find('=');if(eq==std::string::npos)throw std::invalid_argument("expected key=value");
            const auto key=(section.empty()?"":section+".")+trim(line.substr(0,eq));
            if(!assigned.insert(key).second)throw std::invalid_argument("duplicate field: "+key);
            assign(available,key,line.substr(eq+1));
        }catch(const std::exception& e){throw std::invalid_argument(source+":"+std::to_string(number)+": "+e.what());}
    }
    if(in.bad() || (in.fail()&&!in.eof()))throw std::invalid_argument(source+": cannot read configuration");
    return c;
}
void apply_configuration_override(SimulationConfiguration& c,const std::string& key,const std::string& value) {
    auto available=fields(c);assign(available,trim(key),value);
}
ResolvedSimulationConfiguration resolve_configuration(SimulationConfiguration c) {
    auto& d=c.device;const auto& g=d.geometry;
    if(d.spec_profile!="ocp_hbf_v0.7.0")throw std::invalid_argument("device.spec_profile: expected ocp_hbf_v0.7.0");
    g.validate();
    if(!d.selected.zone_size_dlus)d.selected.zone_size_dlus=g.block_group_dlus();
    d.topology.die_to_core_channel=d.topology.resolved_routes(g);
    StaticDeviceDescription description(g,d.capabilities,d.selected,d.topology,d.capacity_policy);
    c.execution.validate(description);c.host.validate();
    validate_host_send_policy(c.host.send_policy);
    policy("execution.read_dispatch.policy",c.execution.read_dispatch.policy,"window");
    if(c.execution.nand_scheduler!="read_priority")
        policy("execution.nand_scheduler.policy",c.execution.nand_scheduler,"fcfs");
    policy("execution.ucie_scheduler.policy",c.execution.ucie_scheduler,"fifo");
    if(c.host.writes==WriteCommandMode::Burst4096 && !d.capabilities.write_burst)
        throw std::invalid_argument("host.write_mode=burst4096 requires device.capabilities.write_burst=true");
    try {(void)RoundRobinInterleaver(c.address.global_base,g.host_channels,g.channel_bytes(),c.address.interleave_bytes);}
    catch(const std::exception& e){throw std::invalid_argument(std::string("address.global_base/interleave_bytes: ")+e.what());}
    // Bound all derived products before runtime allocation or export.
    (void)checked_mul(checked_mul(g.host_channels,d.capabilities.max_pending_write_dlus),dlu_bytes);
    (void)checked_mul(g.host_channels,c.execution.buffers.ingress.payload_bytes);
    (void)checked_mul(g.host_channels,c.execution.buffers.egress.payload_bytes);
    const auto& f=c.faults;
    if(f.target.channel>=g.host_channels || f.target.die>=g.dies_per_channel || f.target.bank>=g.banks_per_die || f.target.block>=g.blocks_per_bank)
        throw std::invalid_argument("faults.channel/die/bank/block: target outside logical geometry");
    for(auto page:f.fail_sequence)if(page>=g.pages_per_block)throw std::invalid_argument("faults.fail_sequence: page outside geometry");
    if(f.persistent_page && *f.persistent_page>=g.pages_per_block)throw std::invalid_argument("faults.persistent_page: page outside geometry");
    if(f.persistent_page && !f.fail_sequence.empty())throw std::invalid_argument("faults: choose fail_sequence OR persistent_page");
    return ResolvedSimulationConfiguration(std::move(c),std::move(description));
}
void write_effective_configuration(std::ostream& out,const ResolvedSimulationConfiguration& resolved) {
    auto c=resolved.values();const auto& g=c.device.geometry;const auto& speed=ucie_speed_profile(c.execution.ucie.speed_grade);
    out<<"# HBF EventLab effective configuration, format 1; OCP profile is a modeled subset.\n"
       <<"# Fixed Tick = 1 ps. Timing/overhead/buffer defaults are uncalibrated model choices.\n"
       <<"# Derived effective_GBps_per_direction_per_module = "<<speed.effective_gbytes_per_second<<"\n"
       <<"# Derived max_ucie_modules = "<<speed.max_modules<<"; max_stack_height = "<<speed.max_stack_height<<"\n"
       <<"# Derived channel_bytes = "<<g.channel_bytes()<<"; total_logical_bytes = "<<checked_mul(g.channel_bytes(),g.host_channels)<<"\n"
       <<"# Derived assembly_payload_bytes_per_channel = "<<checked_mul(c.device.capabilities.max_pending_write_dlus,dlu_bytes)<<"\n"
       <<"# Fixed bank_operation_slots = 1; cache_pages_per_bank = 2; Core Channel = ideal\n";
    std::string section;
    for(const auto& f:fields(c)) {
        const auto dot=f.first.rfind('.');const auto next=f.first.substr(0,dot);
        if(next!=section){section=next;out<<"\n["<<section<<"]\n";}
        out<<f.first.substr(dot+1)<<" = "<<f.second.get()<<"\n";
    }
}
void write_configuration_summary(std::ostream& out,const ResolvedSimulationConfiguration& resolved) {
    const auto& c=resolved.values();const auto& d=c.device;const auto& g=d.geometry;const auto& e=c.execution;
    out<<"CONFIG channels="<<g.host_channels<<" dies/channel="<<g.dies_per_channel<<" banks/die="<<g.banks_per_die
       <<" pages/block="<<g.pages_per_block<<" blocks/bank="<<g.blocks_per_bank<<" spare/bank="<<g.spare_blocks_per_bank
       <<" base="<<c.address.global_base.value<<" interleave_bytes="<<c.address.interleave_bytes
       <<" command_write_bytes="<<(c.host.writes==WriteCommandMode::Fragment64?host_unit_bytes:dlu_bytes)
       <<" open_dlus="<<c.host.max_open_dlus_per_channel<<" mocs="<<d.selected.mocs_selected
       <<" mocs_supported="<<d.capabilities.mocs_supported<<" pending_limit="<<d.capabilities.max_pending_write_dlus<<"\n"
       <<"CONFIG tick=1ps speed_grade="<<static_cast<std::uint32_t>(e.ucie.speed_grade)
       <<" bandwidth_GBps="<<e.ucie.effective_gbytes_per_second()<<" sense="<<e.media_timing.sense.value
       <<" program="<<e.media_timing.program.value<<" erase="<<e.media_timing.erase.value
       <<" assembly_timeout="<<d.selected.assembly_timeout.value<<" read_window="<<e.read_dispatch.wait.value
       <<" read_threshold="<<e.read_dispatch.threshold<<" host_policy="<<c.host.send_policy
       <<" nand_policy="<<e.nand_scheduler<<" ucie_policy="<<e.ucie_scheduler<<"\n"
       <<"CONFIG retention_fragments="<<c.host.retained_fragments<<" replay_pass_limit="<<c.host.max_replay_passes
       <<" pending_retries="<<c.host.max_pending_retries<<" timeout_retries="<<c.host.max_timeout_retries
       <<" retry_delay="<<c.host.pending_retry_delay.value<<" max_tick="<<format(c.run.max_tick)
       <<" event_limits="<<c.run.events.total_events<<"/"<<c.run.events.events_per_tick<<"/"<<c.run.events.rounds_per_tick<<"\n";
}
std::vector<std::string> diagnose_workload(const ResolvedSimulationConfiguration& c,const std::vector<GlobalRequest>& workload) {
    const auto& v=c.values();
    if(v.host.writes==WriteCommandMode::Fragment64 && v.device.selected.mocs_selected<fragments_per_dlu &&
       std::any_of(workload.begin(),workload.end(),[](const auto& r){return r.op==Op::Write;}))
        return {"WRITE_CREDIT_INSUFFICIENT: 64B writes require 64 live command credits to assemble one DLU; this legal configuration remains runnable for timeout experiments."};
    return {};
}
std::function<ProgramOutcome(const ProgramOperation&)> make_program_outcome(const FaultConfiguration& c) {
    return [c,index=std::size_t{}](const ProgramOperation& op) mutable {
        if(!(op.logical==c.target))return ProgramOutcome::Success;
        if(c.persistent_page && op.physical.page==*c.persistent_page)return ProgramOutcome::FailedReusable;
        if(index<c.fail_sequence.size() && op.physical.page==c.fail_sequence[index]){++index;return ProgramOutcome::FailedReusable;}
        return ProgramOutcome::Success;
    };
}
}
