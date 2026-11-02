#include "mailbox_bridge.hpp"
#include "common.cuh"
#include "hbflab/async_device.cuh"
#include "hbflab/live_runtime.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstring>
#include <stdexcept>
#include <vector>

using json=nlohmann::ordered_json;
using namespace hbflab;
namespace {
constexpr const char* selected="blk.10.ffn_norm.weight";
constexpr unsigned trace_capacity=131072;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
struct State {
    int mode=0,step=-1;bool configured=false,finished=false;
    std::thread::id owner;
    std::filesystem::path output;
    const float* weight=nullptr;
    std::vector<float> before;
    AsyncAccessTrace* traces=nullptr;
    unsigned used=0,expected=0,handled=0;
    std::unique_ptr<LiveTimingService> service;
    json calls=json::array(),steps=json::array();
    ~State(){service.reset();if(traces)cudaFree(traces);}
    void current() const {
        if(!configured || finished || owner!=std::this_thread::get_id())throw std::runtime_error("M1 owner/lifecycle mismatch");
    }
} state;
void save(const char* name,const json& value) {
    std::ofstream f(state.output/name);f<<value.dump(2)<<'\n';f.close();
    if(!f)throw std::runtime_error("M1 report write failed");
}
// The reduction and expression order follow upstream norm.cu's
// rms_norm_f32<256,1,true,true,false,false> (llama.cpp MIT license).
// Only the gamma read is replaced by the completion-gated typed load.
__global__ void m1_rms_norm(AsyncDeviceView view,const float* x,const float* gamma,float* dst,
                          float eps,bool modeled,AsyncAccessTrace* traces) {
    const unsigned i=threadIdx.x,row=blockIdx.x;
    const float4 xs=reinterpret_cast<const float4*>(x+row*1024)[i];
    __shared__ float sums[32];
    float tmp=0.0f;
    tmp+=xs.x*xs.x+xs.y*xs.y+xs.z*xs.z+xs.w*xs.w;
    tmp=block_reduce<block_reduce_method::SUM,256>(tmp,sums);
    const float scale=rsqrtf(tmp/1024+eps);
    float4 result=make_float4(scale*xs.x,scale*xs.y,scale*xs.z,scale*xs.w);
    float4 weight{};
    if(modeled) {
        AsyncDeviceClient client(view);
        const auto status=client.load(reinterpret_cast<const float4*>(gamma)+i,weight,traces+row*256+i);
        if(status!=Status::Ok) {
            const float bad=__int_as_float(0x7fffffff);
            reinterpret_cast<float4*>(dst+row*1024)[i]=make_float4(bad,bad,bad,bad);
            return;
        }
    }else weight=reinterpret_cast<const float4*>(gamma)[i];
    result.x*=weight.x;result.y*=weight.y;result.z*=weight.z;result.w*=weight.w;
    reinterpret_cast<float4*>(dst+row*1024)[i]=result;
}
}
extern "C" void hbflab_m1_configure(const char* mode,const char* output) {
    if(state.configured)throw std::runtime_error("M1 is one persistent inference session per process");
    const std::string name=mode;
    if(name!="native" && name!="bypass" && name!="modeled" && name!="fault")throw std::invalid_argument("unknown M1 mode");
    state.mode=name=="native"?0:name=="bypass"?1:name=="modeled"?2:3;
    state.owner=std::this_thread::get_id();state.output=output;state.configured=true;
    if(state.mode>=2)ck(cudaMalloc(reinterpret_cast<void**>(&state.traces),trace_capacity*sizeof(AsyncAccessTrace)));
}
extern "C" void hbflab_m1_step_begin(int step) {
    state.current();
    if(step!=state.step+1 || step>=128)throw std::runtime_error("M1 step sequence/bound mismatch");
    state.step=step;state.expected=state.handled=0;
}
extern "C" void hbflab_m1_graph_node(const ggml_tensor* t) {
    if(!state.configured || state.mode==0 || state.step<0)return;
    state.current();
    for(const auto* src:t->src)if(src && std::strcmp(src->name,selected)==0) {
        if(t->op!=GGML_OP_MUL)throw std::runtime_error("M1 selected weight reached an unsupported graph op");
        ++state.expected;
    }
}
bool hbflab_m1_rms_norm(const ggml_tensor* x,const ggml_tensor* mul,const ggml_tensor* add,
                      float* dst,int ncols,int nrows,int nchannels,int nsamples,float eps,cudaStream_t stream) {
    if(!state.configured || state.mode==0 || !mul || std::strcmp(mul->name,selected)!=0)return false;
    state.current();
    if(state.step<0 || add || ncols!=1024 || nrows<1 || nrows>256 || nchannels!=1 || nsamples!=1 ||
       x->type!=GGML_TYPE_F32 || mul->type!=GGML_TYPE_F32 || !ggml_is_contiguous(x) || !ggml_is_contiguous(mul) ||
       mul->ne[0]!=1024 || mul->ne[1]!=1 || mul->ne[2]!=1 || mul->ne[3]!=1 ||
       reinterpret_cast<uintptr_t>(x->data)%16 || reinterpret_cast<uintptr_t>(mul->data)%16 || reinterpret_cast<uintptr_t>(dst)%16)
        throw std::runtime_error("M1 selected RMSNorm shape/type/layout unsupported");
    if(state.used+static_cast<unsigned>(nrows)*256>trace_capacity)throw std::runtime_error("M1 trace budget exhausted");
    const auto* gamma=static_cast<const float*>(mul->data);
    if(!state.weight) {
        ck(cudaStreamSynchronize(stream));
        state.weight=gamma;state.before.resize(1024);
        ck(cudaMemcpy(state.before.data(),gamma,4096,cudaMemcpyDeviceToHost));
        if(state.mode>=2) {
            AsyncRuntimeOptions options;options.timeout_ns=1000000000;options.service_timeout_ns=10000000000ull;
            options.audit_limit=trace_capacity;options.backend.max_events_per_batch=10000000;
            if(state.mode==3)options.backend.max_events_per_batch=100;
            state.service=std::make_unique<LiveTimingService>(options,512,2);
            state.service->register_readonly(gamma,4096);state.service->start();
        }
    }else if(state.weight!=gamma)throw std::runtime_error("M1 borrowed tensor moved during session");
    ++state.handled;
    for(int row=0;row<nrows;row+=2) {
        const unsigned rows=static_cast<unsigned>(std::min(2,nrows-row));
        const auto* input=static_cast<const float*>(x->data)+row*1024;
        auto* output=dst+row*1024;
        auto* trace=state.traces?state.traces+state.used:nullptr;
        bool modeled=state.mode>=2;
        std::vector<void*> args{&input,&gamma,&output,&eps,&modeled,&trace};
        std::uint64_t launch=0;
        if(modeled)launch=state.service->launch({rows,256},reinterpret_cast<const void*>(m1_rms_norm),args,stream);
        else {
            AsyncDeviceView view{};std::vector<void*> actual{&view};actual.insert(actual.end(),args.begin(),args.end());
            ck(cudaLaunchKernel(reinterpret_cast<const void*>(m1_rms_norm),dim3(rows),dim3(256),actual.data(),0,stream));
        }
        state.calls.push_back({{"step",state.step},{"phase",state.step==0?"prefill":"decode"},
            {"launch",launch},{"row_base",row},{"rows",rows},{"trace_base",state.used},{"accesses",rows*256},
            {"weight",selected},{"weight_address",reinterpret_cast<uintptr_t>(gamma)},{"modeled",modeled}});
        state.used+=rows*256;
    }
    return true;
}
extern "C" void hbflab_m1_step_end() {
    state.current();
    if(state.mode && (state.expected!=1 || state.handled!=state.expected))throw std::runtime_error("M1 incomplete selected-weight coverage");
    if(state.service)state.service->synchronize();
    state.steps.push_back({{"step",state.step},{"expected_ops",state.expected},{"handled_ops",state.handled}});
}
extern "C" void hbflab_m1_finish() {
    state.current();
    json report={{"mode",state.mode==0?"native":state.mode==1?"bypass":state.mode==2?"modeled":"fault"},
        {"target",selected},{"range_bytes",4096},{"steps",state.steps},{"calls",state.calls},
        {"selected_accesses",state.used},{"unselected_policy","native_unmodeled"},
        {"semantics","each selected 16-byte program load is modeled; not a hardware cache-miss trace"}};
    if(state.service) {
        state.service->finish();const auto s=state.service->statistics();const auto b=s.backend;const auto d=b.device;
        if(!state.service->drained() || s.submitted!=state.used || s.completed!=state.used || s.audit_dropped)
            throw std::runtime_error("M1 accounting or drain failure");
        report["runtime"]={{"submitted",s.submitted},{"completed",s.completed},{"grants",s.grants},
            {"launches_submitted",s.launches_submitted},{"launches_completed",s.launches_completed},
            {"peak_inflight",s.peak_inflight},{"peak_active_launches",s.peak_active_launches},
            {"gpu_epoch_ns",s.gpu_epoch_ns},{"model_epoch_ps",s.model_epoch_ps},{"model_now_ps",s.model_now_ps},
            {"drained",true},{"audit_dropped",s.audit_dropped}};
        report["backend"]={{"requests",b.requests},{"sectors",b.sectors},{"backpressure",b.backpressure},
            {"peak_registered_globals",b.peak_registered_globals},{"senses",d.senses},{"cache_hits",d.cache_hits},
            {"programs",d.programs},{"erases",d.erases},{"accepted",d.accepted},{"delivered",d.delivered}};
        std::ofstream host(state.output/"hbf-host.jsonl");
        for(const auto& a:state.service->audit())host<<json({{"launch",a.launch},{"producer",a.producer},{"ticket",a.ticket},
            {"arrival_ns",a.arrival_ns},{"arrival_ps",a.arrival_ps},{"end_ps",a.end_ps},{"dispatch",a.dispatch},
            {"floor_ns",a.grant_floor_ns},{"sectors",a.sectors},{"address",a.first_address},{"bytes",a.first_bytes},{"span_count",a.span_count}}).dump()<<'\n';
        host.close();if(!host)throw std::runtime_error("M1 host audit write failed");
        std::vector<AsyncAccessTrace> traces(state.used);ck(cudaMemcpy(traces.data(),state.traces,traces.size()*sizeof(AsyncAccessTrace),cudaMemcpyDeviceToHost));
        std::ofstream device(state.output/"hbf-device.jsonl");
        for(unsigned i=0;i<traces.size();++i) {
            const auto& t=traces[i];const auto& a=t.access;
            if(a.status!=Status::Ok || t.wanted_ns>t.granted_ns || a.arrival_ns<t.floor_ns || a.completion_seen_ns<a.arrival_ns ||
               a.resumed_ns<a.completion_seen_ns || a.resumed_ns-a.arrival_ns<a.delay_ns || !a.modeled_ps)
                throw std::runtime_error("M1 dependency wait evidence invalid");
            device<<json({{"index",i},{"session",t.session},{"launch",t.launch},{"producer",t.producer},{"ticket",t.ticket},
                {"address",t.address},{"bytes",t.bytes},{"wanted_ns",t.wanted_ns},{"granted_ns",t.granted_ns},{"floor_ns",t.floor_ns},
                {"arrival_ns",a.arrival_ns},{"completion_seen_ns",a.completion_seen_ns},{"resumed_ns",a.resumed_ns},
                {"modeled_ps",a.modeled_ps},{"delay_ns",a.delay_ns},{"status",static_cast<unsigned>(a.status)}}).dump()<<'\n';
        }
        device.close();if(!device)throw std::runtime_error("M1 device audit write failed");
        state.service.reset();ck(cudaFree(state.traces));state.traces=nullptr;
    }
    if(state.weight) {
        std::vector<float> after(1024);ck(cudaMemcpy(after.data(),state.weight,4096,cudaMemcpyDeviceToHost));
        if(std::memcmp(after.data(),state.before.data(),4096)!=0)throw std::runtime_error("M1 borrowed weight changed");
        std::ofstream f(state.output/"borrowed-weight.f32",std::ios::binary);f.write(reinterpret_cast<const char*>(after.data()),4096);f.close();
        if(!f)throw std::runtime_error("M1 weight evidence write failed");report["borrowed_unchanged"]=true;
    }
    report["status"]="success";save("hbf-summary.json",report);state.finished=true;
}
extern "C" void hbflab_m1_abort() {
    state.service.reset();
    if(state.traces){cudaFree(state.traces);state.traces=nullptr;}
}
