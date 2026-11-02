#include "integrated_profile.hpp"
#include "integrated_geometry.hpp"
#include "instrumented_bridge.hpp"
#include "paged_bridge.hpp"
#include "weights_bridge.hpp"
#include "common.cuh"
#include "hbflab/async_device.cuh"
#include "hbflab/live_runtime.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <array>

using json=nlohmann::ordered_json;
using namespace hbflab;
namespace {
const char* names[2]{};
unsigned sizes[2]{};
unsigned trace_capacity=0;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
void need(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct State {
    int mode=0,step=-1;bool configured=false,finished=false,bound=false;
    std::thread::id owner;
    std::filesystem::path output;
    const void* weight[2]{};
    std::vector<unsigned char> before[2];
    AsyncAccessTrace* traces=nullptr;
    unsigned used=0,peak_traces=0,expected[2]{},handled[2]{};
    std::uint64_t total=0,launches=0,rows[2]{},host_records=0,device_records=0;
    double setup_ms=0;
    std::unique_ptr<LiveTimingService> service;
    std::ofstream host,device,calls,steps;
    ~State(){service.reset();if(traces)cudaFree(traces);}
    void current() const {
        need(configured && !finished && owner==std::this_thread::get_id(),"M2 owner/lifecycle mismatch");
    }
} state;
void line(std::ofstream& f,const json& value){f<<value.dump()<<'\n';need(bool(f),"M2 audit write failed");}
void save(const char* name,const json& value) {
    std::ofstream f(state.output/name);f<<value.dump(2)<<'\n';f.close();need(bool(f),"M2 report write failed");
}
int which(const ggml_tensor* t) {
    if(!t)return -1;
    for(int i=0;i<2;++i)if(std::strcmp(t->name,names[i])==0)return i;
    // Reject unnamed aliases overlapping a selected range as unsupported;
    // registration is a byte-range contract, not only a tensor-name filter.
    const auto p=reinterpret_cast<uintptr_t>(t->data);
    if(state.bound && p)for(int i=0;i<2;++i) {
        const auto b=reinterpret_cast<uintptr_t>(state.weight[i]);const auto n=ggml_nbytes(t);
        if(p>=b?p-b<sizes[i]:b-p<n)return i;
    }
    return -1;
}
// Derived from pinned upstream mul_mat_vec_f<half,float,1,256> (MIT).
// Both reference and adapted paths use this F32 accumulation order in both
// phases. The selected row is gated in the same kernel that computes it.
template<bool selected> __global__ void m2_down(AsyncDeviceView view,const half* w,const float* x,float* dst,
                                               bool modeled,AsyncAccessTrace* traces,unsigned hidden,unsigned ffn) {
    const unsigned token=selected?blockIdx.x:blockIdx.x/(hidden-1);
    const unsigned row=selected?0:1+blockIdx.x%(hidden-1);
    float sum=0;
    __shared__ float sums[32];
    if(threadIdx.x<32)sums[threadIdx.x]=0;
    __syncthreads();
    for(unsigned v=threadIdx.x;v<(ffn/2);v+=256) {
        half2 weight{};bool ok=true;
        const auto* address=reinterpret_cast<const half2*>(w+row*ffn)+v;
        if constexpr(selected) {
            if(modeled) {
                AsyncDeviceClient client(view);
                unsigned packed=0;
                ok=client.warp_load_contiguous(reinterpret_cast<const unsigned*>(address),packed,0xffffffffu,traces+token*(ffn/64)+v/32)==Status::Ok;
                memcpy(&weight,&packed,sizeof(weight));
            }else weight=*address;
        }else weight=*address;
        if(ok) {
            const float2 values=__half22float2(weight);
            const float2 input=reinterpret_cast<const float2*>(x+token*ffn)[v];
            ggml_cuda_mad(sum,values.x,input.x);ggml_cuda_mad(sum,values.y,input.y);
        }
    }
    sum=warp_reduce_sum<32>(sum);
    if((threadIdx.x&31)==0)sums[threadIdx.x/32]=sum;
    __syncthreads();
    if(threadIdx.x<32)sum=warp_reduce_sum<32>(sums[threadIdx.x]);
    if(threadIdx.x==0)dst[token*hidden+row]=sum;
}
void launch(unsigned kind,unsigned count,unsigned row_base,const void* kernel,std::vector<void*>& args,cudaStream_t stream) {
    const unsigned accesses=count*(integrated_profile().ffn/64);
    need(state.used+accesses<=trace_capacity,"M2 per-step audit capacity exceeded");
    std::uint64_t id=0;
    if(state.mode>=2)id=state.service->launch({count,256},kernel,args,stream);
    else {
        AsyncDeviceView view{};std::vector<void*> actual{&view};actual.insert(actual.end(),args.begin(),args.end());
        ck(cudaLaunchKernel(kernel,dim3(count),dim3(256),actual.data(),0,stream));
    }
    line(state.calls,{{"step",state.step},{"launch",id},{"kind",kind},{"weight",names[kind]},
        {"address",reinterpret_cast<uintptr_t>(state.weight[kind])},{"range_bytes",sizes[kind]},
        {"row_base",row_base},{"rows",count},{"trace_base",state.used},{"requests",accesses}});
    state.used+=accesses;state.total+=accesses;++state.launches;state.rows[kind]+=count;
    state.peak_traces=std::max(state.peak_traces,state.used);
}
}
extern "C" void hbflab_m2_configure(const char* mode,const char* output) {
    need(!state.configured,"M2 only supports one session per process");
    const std::string name=mode;
    need(name=="native" || name=="bypass" || name=="modeled" || name=="fault","unknown M2 mode");
    state.mode=name=="native"?0:name=="bypass"?1:name=="modeled"?2:3;
    const auto& p=integrated_profile();names[0]=p.norm_name.c_str();names[1]=p.down_name.c_str();sizes[0]=p.hidden*4;sizes[1]=p.ffn*2;trace_capacity=p.trace_records;
    state.owner=std::this_thread::get_id();state.output=output;state.configured=true;
    state.calls.open(state.output/"hbf-calls.jsonl");state.steps.open(state.output/"hbf-steps.jsonl");
    state.host.open(state.output/"hbf-host.jsonl");state.device.open(state.output/"hbf-device.jsonl");
    need(state.calls && state.steps && state.host && state.device,"cannot open M2 logs");
}
extern "C" void hbflab_m2_bind(const ggml_tensor* norm,const ggml_tensor* down) {
    state.current();need(!state.bound,"M2 already bound");
    const ggml_tensor* tensors[]={norm,down};
    const auto start=std::chrono::steady_clock::now();
    for(int i=0;i<2;++i) {
        const auto* t=tensors[i];
        need(t && std::strcmp(t->name,names[i])==0 && ggml_is_contiguous(t) && t->ne[2]==1 && t->ne[3]==1 &&
             t->type==(i?GGML_TYPE_F16:GGML_TYPE_F32) && t->ne[0]==(i?integrated_profile().ffn:integrated_profile().hidden) && t->ne[1]==(i?integrated_profile().hidden:1) &&
             reinterpret_cast<uintptr_t>(t->data)%16==0,"M2 selected tensor layout mismatch");
        state.weight[i]=t->data;state.before[i].resize(sizes[i]);
        ck(cudaMemcpy(state.before[i].data(),t->data,sizes[i],cudaMemcpyDeviceToHost));
    }
    state.bound=true;
    if(state.mode>=2) {
        ck(cudaMalloc(reinterpret_cast<void**>(&state.traces),trace_capacity*sizeof(AsyncAccessTrace)));
        AsyncRuntimeOptions options;options.timeout_ns=1000000000;options.service_timeout_ns=10000000000ull;
        options.backend.geometry=m6_geometry();
        options.audit_limit=integrated_profile().audit_records;options.backend.max_events_per_batch=state.mode==3?100:integrated_profile().max_events;
        state.service=std::make_unique<LiveTimingService>(options,512,2);
        for(int i=0;i<2;++i)state.service->register_readonly(state.weight[i],sizes[i]);
        m4_begin(state.mode,state.service.get(),state.output.string().c_str());
        // Snapshot initialization separately from inference statistics.
        const auto init=state.service->statistics().backend.device;
        save("hbf-initialization.json",{{"programs",init.programs},{"erases",init.erases},{"accepted",init.accepted},{"delivered",init.delivered}});
        state.service->start();
    }
    if(state.mode==1)m4_begin(state.mode,nullptr,state.output.string().c_str());
    m5_begin(state.service.get(),state.mode,state.output.string().c_str());
    state.setup_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
extern "C" void hbflab_m2_step_begin(int step) {
    state.current();need(state.bound && step==state.step+1,"M2 step sequence mismatch");
    m4_step_begin(step);m5_step();
    state.step=step;state.used=0;
    for(int i=0;i<2;++i)state.expected[i]=state.handled[i]=0;
}
extern "C" void hbflab_m2_graph_node(const ggml_tensor* t) {
    if(!state.configured || state.mode==0 || state.step<0)return;
    state.current();
    for(const auto* src:t->src) {
        const int i=which(src);if(i<0)continue;
        need(src->data==state.weight[i] && std::strcmp(src->name,names[i])==0 &&
             t->op==(i?GGML_OP_MUL_MAT:GGML_OP_MUL),"M2 selected range reached unsupported graph op or alias");
        ++state.expected[i];
    }
}
bool hbflab_m2_precision(const ggml_tensor* w,ggml_tensor* dst) {
    if(state.configured && which(w)==1) {
        const int32_t precision=GGML_PREC_F32;std::memcpy(dst->op_params,&precision,sizeof(precision));
        return true;
    }
    return false;
}
bool hbflab_m2_selected_mat(const ggml_tensor* t) {
    return state.configured && t->op==GGML_OP_MUL_MAT && which(t->src[0])==1;
}
bool hbflab_m2_rms_norm(const ggml_tensor*,const ggml_tensor* mul,const ggml_tensor*,float*,int,int,int,int,float,cudaStream_t) {
    if(state.configured && which(mul)==0)throw std::runtime_error("M6 selected norm unexpectedly fused");return false;
}
bool hbflab_m2_mul_mat(const ggml_tensor* w,const ggml_tensor* x,ggml_tensor* dst,cudaStream_t stream) {
    if(!state.configured || !state.mode || which(w)!=1)return false;
    state.current();unsigned hidden=integrated_profile().hidden,ffn=integrated_profile().ffn;
    need(state.step>=0 && w->data==state.weight[1] && w->type==GGML_TYPE_F16 && w->ne[0]==ffn && w->ne[1]==hidden &&
         w->ne[2]==1 && w->ne[3]==1 && ggml_is_contiguous(w) &&
         x->type==GGML_TYPE_F32 && x->ne[0]==ffn && x->ne[1]>=1 && x->ne[1]<=integrated_profile().ubatch && x->ne[2]==1 && x->ne[3]==1 && ggml_is_contiguous(x) &&
         dst->type==GGML_TYPE_F32 && dst->ne[0]==hidden && dst->ne[1]==x->ne[1] && dst->ne[2]==1 && dst->ne[3]==1 && ggml_is_contiguous(dst),
         "M2 F16 matmul layout unsupported");
    ++state.handled[1];const auto* weights=static_cast<const half*>(w->data);
    // These disjoint output rows never consume the registered row-zero bytes.
    m2_down<false><<<static_cast<unsigned>(x->ne[1])*(hidden-1),256,0,stream>>>({},weights,static_cast<const float*>(x->data),static_cast<float*>(dst->data),false,nullptr,hidden,ffn);
    ck(cudaGetLastError());
    for(int row=0;row<x->ne[1];row+=2) {
        unsigned count=std::min<int>(2,static_cast<int>(x->ne[1])-row);
        const auto* input=static_cast<const float*>(x->data)+row*ffn;auto* output=static_cast<float*>(dst->data)+row*hidden;
        auto* trace=state.traces?state.traces+state.used:nullptr;bool modeled=state.mode>=2;
        std::vector<void*> args{&weights,&input,&output,&modeled,&trace,&hidden,&ffn};
        launch(1,count,row,reinterpret_cast<const void*>(m2_down<true>),args,stream);
    }
    return true;
}
extern "C" void hbflab_m2_step_end() {
    m4_step_end();
    state.current();
    if(state.mode)for(int i=0;i<2;++i)need(state.expected[i]>0 && state.handled[i]==state.expected[i],"M2 incomplete selected-range coverage");
    if(state.service) {
        state.service->synchronize();
        auto host=state.service->take_audit();need(host.size()==state.used+m5_step_requests(),"M2 host audit missing or overflowed");
        for(const auto& a:host)line(state.host,{{"step",state.step},{"launch",a.launch},{"producer",a.producer},{"ticket",a.ticket},
            {"arrival_ns",a.arrival_ns},{"arrival_ps",a.arrival_ps},{"end_ps",a.end_ps},{"dispatch",a.dispatch},
            {"floor_ns",a.grant_floor_ns},{"sectors",a.sectors},{"address",a.first_address},{"bytes",a.first_bytes},{"span_count",a.span_count}});
        state.host_records+=host.size();
        std::vector<AsyncAccessTrace> traces(state.used);
        ck(cudaMemcpy(traces.data(),state.traces,traces.size()*sizeof(AsyncAccessTrace),cudaMemcpyDeviceToHost));
        for(unsigned i=0;i<traces.size();++i) {
            const auto& t=traces[i];const auto& a=t.access;
            need(a.status==Status::Ok && t.wanted_ns<=t.granted_ns && a.arrival_ns>=t.floor_ns && a.completion_seen_ns>=a.arrival_ns &&
                 a.resumed_ns>=a.completion_seen_ns && a.resumed_ns-a.arrival_ns>=a.delay_ns && a.modeled_ps,"M2 dependency evidence invalid");
            line(state.device,{{"step",state.step},{"index",i},{"session",t.session},{"launch",t.launch},{"producer",t.producer},{"ticket",t.ticket},
                {"address",t.address},{"bytes",t.bytes},{"wanted_ns",t.wanted_ns},{"granted_ns",t.granted_ns},{"floor_ns",t.floor_ns},
                {"arrival_ns",a.arrival_ns},{"completion_seen_ns",a.completion_seen_ns},{"resumed_ns",a.resumed_ns},
                {"modeled_ps",a.modeled_ps},{"delay_ns",a.delay_ns},{"status",static_cast<unsigned>(a.status)}});
        }
        state.device_records+=traces.size();
    }
    line(state.steps,{{"step",state.step},{"expected",state.expected},{"handled",state.handled},{"requests",state.used}});
}
extern "C" void hbflab_m2_finish() {
    state.current();json report={{"status","success"},{"mode",state.mode},{"matrix_precision","upstream per-token MMVF with GGML_PREC_F32 in all three modes; distinct from M0 default"},{"profile_sha256",integrated_profile().hash},{"range_bytes",sizes[0]+sizes[1]},{"steps",state.step+1},
        {"selected_requests",state.total+m5_total_requests()},{"helper_requests",state.total},{"launches",state.launches},{"helper_rows",state.rows},{"setup_ms",state.setup_ms},
        {"trace_capacity",trace_capacity},{"peak_trace_records",state.peak_traces},{"producer_slots",512},{"launch_slots",2},
        {"host_records",state.host_records},{"device_records",state.device_records},
        {"scope",{{"rms_norm",names[0]},{"f16_matmul",names[1]},{"f16_byte_offset",0},{"f16_bytes",sizes[1]}}},
        {"unselected_weights_policy","native_unmodeled; other matrix rows use controlled arithmetic"},{"kv_policy",state.mode?"paged F16 KV through Capacity":"native GPU KV reference"},
        {"semantics","M5 norm weights: original NVCC PTX scalar reads, one request per active thread; down: original M2 128B warp spans; native activation role is explicit"}};
    m5_finish();m4_finish();
    if(state.service) {
        state.service->finish();const auto s=state.service->statistics();const auto b=s.backend;const auto d=b.device;
        need(state.service->drained() && s.submitted==state.total+m5_total_requests() && s.completed==state.total+m5_total_requests() && !s.audit_dropped && state.service->take_audit().empty(),"M2 accounting/drain failure");
        report["runtime"]={{"submitted",s.submitted},{"completed",s.completed},{"grants",s.grants},
            {"launches_submitted",s.launches_submitted},{"launches_completed",s.launches_completed},
            {"peak_inflight",s.peak_inflight},{"peak_active_launches",s.peak_active_launches},
            {"gpu_epoch_ns",s.gpu_epoch_ns},{"model_epoch_ps",s.model_epoch_ps},{"model_now_ps",s.model_now_ps},
            {"drained",true},{"audit_dropped",s.audit_dropped},{"media_submitted",s.media_submitted},{"media_completed",s.media_completed}};
        report["backend"]={{"requests",b.requests},{"sectors",b.sectors},{"backpressure",b.backpressure},
            {"interface","SimulationSession -> HostDriver -> DeviceDriver"},{"input_closed",b.input_closed},
            {"registered_globals",b.registered_globals},{"completed_globals",b.completed_globals},{"registered_fragments",b.registered_fragments},
            {"host_logical_success",b.host.logical_success},{"host_logical_failed",b.host.logical_failed},
            {"host_replay_commands",b.host.replay_commands},{"host_attempt_errors",b.host.attempt_errors},
            {"resident_fragment_limit",256},{"peak_resident_fragments",b.peak_resident_fragments},
            {"peak_registered_globals",b.peak_registered_globals},{"senses",d.senses},{"cache_hits",d.cache_hits},
            {"writebacks",b.writebacks},{"replayed_pages",b.replayed_pages},{"programs",d.programs},{"erases",d.erases},{"accepted",d.accepted},{"delivered",d.delivered}};
        state.service.reset();ck(cudaFree(state.traces));state.traces=nullptr;
    }
    for(int i=0;i<2;++i) {
        std::vector<unsigned char> after(sizes[i]);ck(cudaMemcpy(after.data(),state.weight[i],sizes[i],cudaMemcpyDeviceToHost));
        need(after==state.before[i],"M2 borrowed bytes changed");
    }
    report["borrowed_unchanged"]=true;
    state.host.close();state.device.close();state.calls.close();state.steps.close();
    need(state.host && state.device && state.calls && state.steps,"M2 log close failed");
    save("hbf-summary.json",report);state.finished=true;
}
extern "C" void hbflab_m2_abort() {
    m5_abort();m4_abort();state.service.reset();if(state.traces){cudaFree(state.traces);state.traces=nullptr;}
}

bool hbflab_m5_mul(ggml_tensor* t,cudaStream_t stream) {
    if(!state.configured || t->op!=GGML_OP_MUL || which(t->src[1])!=0)return false;
    state.current();const auto* x=t->src[0];const auto* w=t->src[1];
    need(state.step>=0 && x->type==GGML_TYPE_F32 && w->type==GGML_TYPE_F32 && t->type==GGML_TYPE_F32 &&
        ggml_is_contiguous(x) && ggml_is_contiguous(w) && ggml_is_contiguous(t) &&
        x->ne[0]==integrated_profile().hidden && x->ne[1]>=1 && x->ne[1]<=integrated_profile().ubatch && x->ne[2]==1 && x->ne[3]==1 &&
        ggml_nelements(w)==integrated_profile().hidden && ggml_nelements(t)==ggml_nelements(x) && w->data==state.weight[0],"M5 multiply profile mismatch");
    ++state.handled[0];m5_mul(static_cast<const float*>(x->data),static_cast<const float*>(w->data),static_cast<float*>(t->data),static_cast<unsigned>(x->ne[1]),stream);return true;
}
