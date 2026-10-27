// Optional experiment adapter. The external CUDA executor remains unmodified.
#include "access_plan.hpp"
#include "hbf_gpu/qwen.hpp"
#include "hbf_eventlab/statistics/event_stream.hpp"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>

using namespace hbf_eventlab;
namespace {
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point t){return std::chrono::duration<double>(Clock::now()-t).count();}
void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
std::string quote(const std::string& s){std::ostringstream o;o<<'"';for(unsigned char c:s){
    if(c=='"'||c=='\\')o<<'\\'<<char(c);else if(c<32)o<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
    else o<<char(c);}o<<'"';return o.str();}
std::uint64_t number(const std::string& s){require(!s.empty()&&s.find_first_not_of("0123456789")==std::string::npos,"expected unsigned decimal");return std::stoull(s);}
struct Args {
    std::string mode,model,prefix,config,detail="full",profile="off",tensor="model.layers.0.self_attn.q_proj.weight";
    std::uint64_t offset=0,bytes=8192,count=1,compute=1000,tail=5000,chunk=4096,device=0;
    std::optional<std::uint64_t> resident;
};
Args arguments(int argc,char** argv){Args a;std::set<std::string> seen;
    for(int i=1;i<argc;i+=2){require(i+1<argc,"options require values");std::string k=argv[i],v=argv[i+1];require(seen.insert(k).second,"duplicate option: "+k);
        if(k=="--mode")a.mode=v;else if(k=="--model")a.model=v;else if(k=="--output-prefix")a.prefix=v;
        else if(k=="--config")a.config=v;else if(k=="--tensor")a.tensor=v;
        else if(k=="--detail")a.detail=v;
        else if(k=="--profile")a.profile=v;
        else if(k=="--offset")a.offset=number(v);else if(k=="--bytes")a.bytes=number(v);
        else if(k=="--max-new")a.count=number(v);else if(k=="--compute-ps")a.compute=number(v);
        else if(k=="--tail-ps")a.tail=number(v);else if(k=="--chunk")a.chunk=number(v);
        else if(k=="--resident")a.resident=number(v);else if(k=="--device")a.device=number(v);
        else throw std::invalid_argument("unknown option: "+k);
    }
    require((a.mode=="native"||a.mode=="hbf")&&!a.model.empty()&&!a.prefix.empty(),"required: --mode native|hbf --model DIR --output-prefix PATH");
    require(a.count>0&&a.count<128&&a.device<=INT_MAX,"invalid generation count/device");
    require(a.detail=="full"||a.detail=="off","detail must be full or off");
    require(a.profile=="on"||a.profile=="off","profile must be on or off");
    require(a.profile=="off"||a.mode=="hbf","profiling requires hbf mode");return a;
}
std::map<std::string,std::uint64_t> inventory(const std::string& directory){
    std::ifstream f(directory+"/layout.txt");require(bool(f),"cannot open layout.txt");
    std::map<std::string,std::uint64_t> result;std::string name;std::uint64_t offset,bytes,dimension;unsigned rank;
    while(f>>name){require(bool(f>>offset>>bytes>>rank)&&rank>0&&rank<=2,"bad layout header");
        std::uint64_t product=4;for(unsigned i=0;i<rank;++i){require(bool(f>>dimension),"bad layout dimension");product=checked_mul(product,dimension);}
        require(bytes==product&&result.emplace(name,bytes).second,"bad/duplicate tensor size");}
    require(f.eof(),"malformed layout");return result;
}
std::vector<unsigned> prompt(const Args& a){std::ifstream f(a.model+"/prompt.ids");require(bool(f),"missing prompt.ids");
    std::vector<unsigned> ids;std::uint64_t t;while(f>>t){require(t<151936,"invalid prompt token");ids.push_back(unsigned(t));}
    require(f.eof()&&!ids.empty()&&ids.size()+a.count<=128,"invalid prompt/context size");return ids;}
struct State {
    std::uint64_t gates=0,selected=0,covered=0,compute=0,init_end=0,inference_end=0,tail_start=0,tail_end=0,clock=0;
    std::uint64_t initialized_bytes=0,parent_globals=0,parent_finished=0,backpressure=0,positions=0;
    bool success=false,tail_complete=false,inference_started=false;std::string error,stop="not_started";std::vector<unsigned> tokens;
    double backend_wall=0,backend_inference_wall=0,inference_wall=0,load_wall=0,initialization_wall=0;
};
struct BackendTimer {State& s;Clock::time_point start=Clock::now();~BackendTimer(){s.backend_wall+=elapsed(start);}};
void add_access(State& s,const TensorAccessResult& r){s.parent_globals+=r.registered_globals;s.parent_finished+=r.finished_globals;s.backpressure+=r.capacity_rejections;}
void checked_close(std::ofstream& f){f.close();require(bool(f),"output write/close failed");}
}
int main(int argc,char** argv){
    const auto total_start=Clock::now();
    try {
        const auto a=arguments(argc,argv);const auto tensors=inventory(a.model);const auto ids=prompt(a);
        require(a.tensor!="lm_head.weight"&&tensors.count(a.tensor),"selected tensor is not used by executor");
        const auto tensor_bytes=tensors.at(a.tensor);
        require(a.offset<=tensor_bytes,"selection offset outside tensor");
        qwen_online::SelectedRange selected{a.tensor,tensor_bytes,a.offset,a.bytes?a.bytes:tensor_bytes-a.offset};selected.validate();
        SimulationConfiguration config;
        if(!a.config.empty()){std::ifstream f(a.config);require(bool(f),"cannot open backend config");config=read_configuration(f,a.config);}
        else {
            config.device.geometry={1,1,2,8,std::max<std::uint64_t>(1,(selected.bytes-1)/(2*8*dlu_bytes)+1),1};
            config.host.max_resident_fragments=64;
        }
        if(a.resident)config.host.max_resident_fragments=*a.resident;
        const auto resolved=resolve_configuration(config);
        // Initial smoke contract: complete pages in one Global contiguous span.
        require(selected.bytes%(dlu_bytes*config.device.geometry.host_channels)==0,"selection must cover a whole channel-page group");
        const TensorRegion region{selected.name,config.address.global_base,selected.bytes,1};
        if(!std::filesystem::path(a.prefix).parent_path().empty())std::filesystem::create_directories(std::filesystem::path(a.prefix).parent_path());
        require(!std::filesystem::exists(a.prefix+".json")&&!std::filesystem::exists(a.prefix+".bin"),"output prefix already exists; choose a fresh prefix");
        require(a.profile=="off"||!std::filesystem::exists(a.prefix+".profile.json"),"runtime profile output already exists");
        std::ofstream effective(a.prefix+".effective.ini");write_effective_configuration(effective,resolved);checked_close(effective);
        std::ofstream plan(a.prefix+".plan.json");plan<<"{\"tensor\":"<<quote(a.tensor)<<",\"tensor_bytes\":"<<tensor_bytes
            <<",\"tensor_offset\":"<<a.offset<<",\"selected_bytes\":"<<selected.bytes<<",\"global_base\":"<<region.base.value
            <<",\"content\":1,\"global_capacity_bytes\":"<<checked_mul(config.device.geometry.channel_bytes(),config.device.geometry.host_channels)
            <<",\"global_chunk_bytes\":"<<a.chunk<<",\"host_unit_bytes\":"<<host_unit_bytes<<",\"dlu_bytes\":"<<dlu_bytes
            <<",\"initialization\":\"ordinary Host write of the registered slice\",\"real_data_owner\":\"CUDA executor\"}\n";checked_close(plan);
        const bool detailed=a.detail=="full";
        std::ofstream gates(a.prefix+".gates.csv"),trace,events,progress(a.prefix+".progress.csv");
        if(detailed){trace.open(a.prefix+".bridge.csv");events.open(a.prefix+".events.jsonl");}
        require(bool(gates)&&bool(progress)&&(!detailed||(bool(trace)&&bool(events))),"cannot create experiment logs");
        gates<<"sequence,tensor,offset,bytes,selected_offset,selected_bytes,compute_ps,arrival_tick,completed_tick,parent,globals,finished,backpressure,success,backend_wall_seconds\n";
        if(detailed)trace<<"action,parent,global,tick\n";
        std::unique_ptr<StatisticsEventStream> stream;
        if(detailed)stream=std::make_unique<StatisticsEventStream>(events,65536);
        progress<<"phase,parent,global_finished,tick,wall_seconds,host_handed,host_responses,resident\n";progress.flush();
        std::unique_ptr<RuntimeProfile> profile;
        if(a.profile=="on")profile=std::make_unique<RuntimeProfile>();
        std::unique_ptr<TensorAccessBridge> bridge;
        State s;std::optional<SessionReport> final_report;HostStatistics hs{};DeviceStatistics ds{};
        auto now=[&](){return bridge?bridge->session().now().value:s.clock;};
        auto advance=[&](std::uint64_t duration){
            if(bridge){BackendTimer timer{s};auto r=bridge->advance_compute(Tick{duration});require(r.success,"compute advance: "+r.reason);}
            else s.clock=checked_add(s.clock,duration);
            s.compute=checked_add(s.compute,duration);
        };
        try {
            if(a.mode=="hbf") {
                bridge=std::make_unique<TensorAccessBridge>(resolved,std::vector<TensorRegion>{region},TensorBridgeConfiguration{a.chunk},stream.get(),profile.get());
                std::uint64_t completed_globals=0;
                bridge->set_trace([&,completed_globals](const TensorBridgeTrace& e) mutable {
                    if(detailed){trace<<e.action<<','<<e.access<<','<<e.global.value<<','<<e.at.value<<'\n';require(bool(trace),"bridge trace write failed");}
                    if(std::strcmp(e.action,"global-finished")==0)++completed_globals;
                    if((std::strcmp(e.action,"global-finished")==0&&completed_globals%128==0)||std::strcmp(e.action,"access-finished")==0){
                        const auto& h=bridge->session().host().statistics();
                        progress<<(s.inference_started?"read":"initialize")<<','<<e.access<<','<<completed_globals<<','<<e.at.value<<','<<std::setprecision(12)<<elapsed(total_start)
                            <<','<<h.handed<<','<<h.responses<<','<<bridge->session().input_capacity().resident<<'\n';progress.flush();require(bool(progress),"progress write failed");}
                });
                const auto init_start=Clock::now();TensorAccessResult init;{BackendTimer timer{s};init=bridge->initialize(region.name);}add_access(s,init);
                s.initialization_wall=elapsed(init_start);
                require(init.success,"initialization failed: "+init.reason);s.initialized_bytes=region.bytes;
            }
            s.init_end=now();
            auto callback=[&](const std::string& name,std::uint64_t at,std::uint64_t count){
                require(tensors.count(name)&&at<=tensors.at(name)&&count<=tensors.at(name)-at,"invalid executor callback range");
                const auto intersection=selected.intersection(name,at,count);
                const auto compute=s.gates?a.compute:0;++s.gates;
                const auto gate_backend_before=s.backend_wall;
                TensorAccessResult access;bool okay=false;std::uint64_t arrival=now();
                auto row=[&]{gates<<s.gates<<','<<name<<','<<at<<','<<count<<','<<(intersection?intersection->first:0)<<','<<(intersection?intersection->second:0)
                    <<','<<compute<<','<<arrival<<','<<now()<<','<<access.id<<','<<access.registered_globals<<','<<access.finished_globals<<','<<access.capacity_rejections<<','<<(okay?1:0)<<','<<std::setprecision(12)<<s.backend_wall-gate_backend_before<<'\n';require(bool(gates),"gate log write failed");};
                try {
                    advance(compute);arrival=now();
                    if(intersection){++s.selected;s.covered=checked_add(s.covered,intersection->second);
                        if(bridge){BackendTimer timer{s};access=bridge->access(region.name,intersection->first,intersection->second);add_access(s,access);require(access.success,"memory gate failed: "+access.reason);}}
                    okay=true;row();
                } catch(...) {row();throw;} // Do not return to a kernel using a failed dependency.
            };
            m6::Options options;options.device=static_cast<int>(a.device);
            auto load_start=Clock::now();m6::Qwen model(a.model,a.prefix+".bin",callback,options);s.load_wall=elapsed(load_start);
            const auto inference_start=Clock::now();const auto backend_before=s.backend_wall;
            s.inference_started=true;
            try {
                std::vector<float> logits;for(auto token:ids){logits=model.step(token,unsigned(s.positions));++s.positions;}
                s.stop="length";
                for(std::uint64_t i=0;i<a.count;++i){require(!logits.empty(),"missing logits");auto next=unsigned(std::max_element(logits.begin(),logits.end())-logits.begin());s.tokens.push_back(next);
                    if(next==151645||next==151643){s.stop="EOS";break;}logits=model.step(next,unsigned(s.positions));++s.positions;}
                // The final gate is before a kernel. Explicitly account for its tail.
                s.tail_start=now();advance(a.tail);s.tail_end=now();s.tail_complete=true;s.inference_end=now();
            } catch(...) {s.inference_wall=elapsed(inference_start);s.backend_inference_wall=s.backend_wall-backend_before;throw;}
            s.inference_wall=elapsed(inference_start);s.backend_inference_wall=s.backend_wall-backend_before;
            model.close();require(s.selected>0,"no executor callback intersected the selection");s.success=true;
        } catch(const std::exception& e){s.error=e.what();s.success=false;s.inference_end=s.inference_started?now():0;}
        if(bridge){
            try {BackendTimer timer{s};final_report=bridge->finish();hs=bridge->session().host().statistics();ds=bridge->session().device().statistics();
                if(final_report->run.status!=RunStatus::Completed||!final_report->run.workload_success||!final_report->run.host.drained()||!final_report->run.device.drained()){
                    s.success=false;if(s.error.empty())s.error=final_report->run.explanation;}}
            catch(const std::exception& e){s.success=false;s.error+="; drain: "+std::string(e.what());}
        }
        const auto stop=final_report?to_string(final_report->run.status):(s.success?"Completed":"AdapterError");
        if(stream)stream->finish(Tick{now()},stop);
        if(profile){std::ofstream output(a.prefix+".profile.json");profile->write_json(output);checked_close(output);}
        checked_close(gates);checked_close(progress);if(detailed){checked_close(trace);checked_close(events);}
        std::ofstream out(a.prefix+".json");out.imbue(std::locale::classic());out<<std::setprecision(17)<<std::boolalpha;
        out<<"{\"schema_version\":1,\"mode\":"<<quote(a.mode)<<",\"success\":"<<s.success<<",\"error\":"<<quote(s.error)<<",\"stop\":"<<quote(s.stop)
           <<",\"detail\":"<<quote(a.detail)<<",\"initialization_wall_seconds\":"<<s.initialization_wall
           <<",\"runtime_profile_enabled\":"<<bool(profile)
           <<",\"run_status\":"<<quote(stop)<<",\"positions\":"<<s.positions<<",\"gates\":"<<s.gates<<",\"selected_gates\":"<<s.selected
           <<",\"covered_access_bytes\":"<<s.covered<<",\"initialized_bytes\":"<<s.initialized_bytes<<",\"globals_registered\":"<<s.parent_globals<<",\"globals_finished\":"<<s.parent_finished
           <<",\"input_backpressure\":"<<s.backpressure<<",\"tick_unit\":\"ps\",\"compute_model\":\"fixed_per_callback_interval\",\"calibrated\":false"
           <<",\"compute_interval_ps\":"<<a.compute<<",\"compute_total_ps\":"<<s.compute<<",\"tail_requested_ps\":"<<a.tail<<",\"tail_start\":"<<s.tail_start<<",\"tail_end\":"<<s.tail_end<<",\"tail_complete\":"<<s.tail_complete
           <<",\"initialization_end_tick\":"<<s.init_end<<",\"inference_end_tick\":"<<s.inference_end<<",\"final_tick\":"<<now()
           <<",\"inference_started\":"<<s.inference_started
           <<",\"simulated_inference_ticks\":"<<(s.inference_started?s.inference_end-s.init_end:0)
           <<",\"wall_seconds\":"<<elapsed(total_start)<<",\"model_load_wall_seconds\":"<<s.load_wall<<",\"inference_wall_seconds\":"<<s.inference_wall
           <<",\"backend_wall_seconds\":"<<s.backend_wall<<",\"inference_backend_wall_seconds\":"<<s.backend_inference_wall
           <<",\"executor_and_dump_wall_seconds\":"<<std::max(0.0,s.inference_wall-s.backend_inference_wall)
           <<",\"host_drained\":"<<(!bridge||(final_report&&final_report->run.host.drained()))<<",\"device_drained\":"<<(!bridge||(final_report&&final_report->run.device.drained()))
           <<",\"input_closed\":"<<(!bridge||bridge->session().input_closed())<<",\"resident_fragments\":"<<(bridge?bridge->session().input_capacity().resident:0)
           <<",\"events_executed\":"<<(final_report?final_report->run.events:0)
           <<",\"host_handed\":"<<hs.handed<<",\"host_accepted\":"<<hs.accepted<<",\"host_responses\":"<<hs.responses<<",\"logical_failed\":"<<hs.logical_failed<<",\"attempt_errors\":"<<hs.attempt_errors
           <<",\"programs\":"<<ds.programs<<",\"senses\":"<<ds.senses<<",\"cache_hits\":"<<ds.cache_hits<<",\"tokens\":[";
        for(std::size_t i=0;i<s.tokens.size();++i){if(i)out<<',';out<<s.tokens[i];}out<<"]}\n";checked_close(out);
        std::cout<<(s.success?"PASS":"FAIL")<<" QWEN_ONLINE mode="<<a.mode<<" selected_gates="<<s.selected<<" globals="<<s.parent_finished<<" backpressure="<<s.backpressure<<" final_tick="<<now()<<" reason="<<s.error<<'\n';
        return s.success?0:2;
    } catch(const std::exception& e){std::cerr<<"QWEN_INPUT_ERROR "<<e.what()<<'\n';return 1;}
}
