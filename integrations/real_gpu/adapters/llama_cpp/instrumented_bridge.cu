#include "instrumented_bridge.hpp"
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
constexpr const char* names[]={"blk.10.ffn_norm.weight","blk.10.ffn_down.weight"};
constexpr unsigned sizes[]={4096,6144};
constexpr unsigned trace_capacity=16384; // 256 prefill rows * (8 + 48) requests
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
// Same arithmetic as the M1/upstream fused RMSNorm. Only eight warp leaders
// publish requests per row; the 256 actual float4 consumers wait together.
__global__ void m2_rms_norm(AsyncDeviceView view,const float* x,const float* gamma,float* dst,
                          float eps,bool modeled,AsyncAccessTrace* traces) {
    const unsigned i=threadIdx.x,row=blockIdx.x;
    const float4 xs=reinterpret_cast<const float4*>(x+row*1024)[i];
    __shared__ float sums[32];
    float tmp=0.0f;tmp+=xs.x*xs.x+xs.y*xs.y+xs.z*xs.z+xs.w*xs.w;
    tmp=block_reduce<block_reduce_method::SUM,256>(tmp,sums);
    const float scale=rsqrtf(tmp/1024+eps);
    float4 result=make_float4(scale*xs.x,scale*xs.y,scale*xs.z,scale*xs.w),weight{};
    if(modeled) {
        AsyncDeviceClient client(view);
        if(client.warp_load_contiguous(reinterpret_cast<const float4*>(gamma)+i,weight,0xffffffffu,traces+row*8+i/32)!=Status::Ok)return;
    }else weight=reinterpret_cast<const float4*>(gamma)[i];
    result.x*=weight.x;result.y*=weight.y;result.z*=weight.z;result.w*=weight.w;
    reinterpret_cast<float4*>(dst+row*1024)[i]=result;
}
// Derived from pinned upstream mul_mat_vec_f<half,float,1,256> (MIT).
// Both reference and adapted paths use this F32 accumulation order in both
// phases. The selected row is gated in the same kernel that computes it.
template<bool selected> __global__ void m2_down(AsyncDeviceView view,const half* w,const float* x,float* dst,
                                               bool modeled,AsyncAccessTrace* traces) {
    const unsigned token=selected?blockIdx.x:blockIdx.x/1023;
    const unsigned row=selected?0:1+blockIdx.x%1023;
    float sum=0;
    __shared__ float sums[32];
    if(threadIdx.x<32)sums[threadIdx.x]=0;
    __syncthreads();
    for(unsigned v=threadIdx.x;v<1536;v+=256) {
        half2 weight{};bool ok=true;
        const auto* address=reinterpret_cast<const half2*>(w+row*3072)+v;
        if constexpr(selected) {
            if(modeled) {
                AsyncDeviceClient client(view);
                unsigned packed=0;
                ok=client.warp_load_contiguous(reinterpret_cast<const unsigned*>(address),packed,0xffffffffu,traces+token*48+v/32)==Status::Ok;
                memcpy(&weight,&packed,sizeof(weight));
            }else weight=*address;
        }else weight=*address;
        if(ok) {
            const float2 values=__half22float2(weight);
            const float2 input=reinterpret_cast<const float2*>(x+token*3072)[v];
            ggml_cuda_mad(sum,values.x,input.x);ggml_cuda_mad(sum,values.y,input.y);
        }
    }
    sum=warp_reduce_sum<32>(sum);
    if((threadIdx.x&31)==0)sums[threadIdx.x/32]=sum;
    __syncthreads();
    if(threadIdx.x<32)sum=warp_reduce_sum<32>(sums[threadIdx.x]);
    if(threadIdx.x==0)dst[token*1024+row]=sum;
}
void launch(unsigned kind,unsigned count,unsigned row_base,const void* kernel,std::vector<void*>& args,cudaStream_t stream) {
    const unsigned accesses=count*(kind?48:8);
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
             t->type==(i?GGML_TYPE_F16:GGML_TYPE_F32) && t->ne[0]==(i?3072:1024) && t->ne[1]==(i?1024:1) &&
             reinterpret_cast<uintptr_t>(t->data)%16==0,"M2 selected tensor layout mismatch");
        state.weight[i]=t->data;state.before[i].resize(sizes[i]);
        ck(cudaMemcpy(state.before[i].data(),t->data,sizes[i],cudaMemcpyDeviceToHost));
    }
    state.bound=true;
    if(state.mode>=2) {
        ck(cudaMalloc(reinterpret_cast<void**>(&state.traces),trace_capacity*sizeof(AsyncAccessTrace)));
        AsyncRuntimeOptions options;options.timeout_ns=1000000000;options.service_timeout_ns=10000000000ull;
        options.audit_limit=trace_capacity;options.backend.max_events_per_batch=state.mode==3?100:100000000;
        state.service=std::make_unique<LiveTimingService>(options,512,2);
        for(int i=0;i<2;++i)state.service->register_readonly(state.weight[i],sizes[i]);
        // Snapshot initialization separately from inference statistics.
        const auto init=state.service->statistics().backend.device;
        save("hbf-initialization.json",{{"programs",init.programs},{"erases",init.erases},{"accepted",init.accepted},{"delivered",init.delivered}});
        state.service->start();
    }
    state.setup_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
extern "C" void hbflab_m2_step_begin(int step) {
    state.current();need(state.bound && step==state.step+1,"M2 step sequence mismatch");
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
bool hbflab_m2_rms_norm(const ggml_tensor* x,const ggml_tensor* mul,const ggml_tensor* add,
                      float* dst,int ncols,int nrows,int nchannels,int nsamples,float eps,cudaStream_t stream) {
    if(!state.configured || !state.mode || which(mul)!=0)return false;
    state.current();
    need(state.step>=0 && !add && ncols==1024 && nrows>=1 && nrows<=256 && nchannels==1 && nsamples==1 &&
         x->type==GGML_TYPE_F32 && mul->type==GGML_TYPE_F32 && ggml_is_contiguous(x) && ggml_is_contiguous(mul) &&
         mul->data==state.weight[0] && ggml_nbytes(mul)==4096 &&
         reinterpret_cast<uintptr_t>(x->data)%16==0 && reinterpret_cast<uintptr_t>(dst)%16==0,"M2 RMSNorm layout unsupported");
    ++state.handled[0];const auto* gamma=static_cast<const float*>(mul->data);
    for(int row=0;row<nrows;row+=2) {
        unsigned count=std::min(2,nrows-row);
        const auto* input=static_cast<const float*>(x->data)+row*1024;auto* output=dst+row*1024;
        auto* trace=state.traces?state.traces+state.used:nullptr;bool modeled=state.mode>=2;
        std::vector<void*> args{&input,&gamma,&output,&eps,&modeled,&trace};
        launch(0,count,row,reinterpret_cast<const void*>(m2_rms_norm),args,stream);
    }
    return true;
}
bool hbflab_m2_mul_mat(const ggml_tensor* w,const ggml_tensor* x,ggml_tensor* dst,cudaStream_t stream) {
    if(!state.configured || !state.mode || which(w)!=1)return false;
    state.current();
    need(state.step>=0 && w->data==state.weight[1] && w->type==GGML_TYPE_F16 && w->ne[0]==3072 && w->ne[1]==1024 &&
         w->ne[2]==1 && w->ne[3]==1 && ggml_is_contiguous(w) &&
         x->type==GGML_TYPE_F32 && x->ne[0]==3072 && x->ne[1]>=1 && x->ne[1]<=256 && x->ne[2]==1 && x->ne[3]==1 && ggml_is_contiguous(x) &&
         dst->type==GGML_TYPE_F32 && dst->ne[0]==1024 && dst->ne[1]==x->ne[1] && dst->ne[2]==1 && dst->ne[3]==1 && ggml_is_contiguous(dst),
         "M2 F16 matmul layout unsupported");
    ++state.handled[1];const auto* weights=static_cast<const half*>(w->data);
    // These disjoint output rows never consume the registered row-zero bytes.
    m2_down<false><<<static_cast<unsigned>(x->ne[1])*1023,256,0,stream>>>({},weights,static_cast<const float*>(x->data),static_cast<float*>(dst->data),false,nullptr);
    ck(cudaGetLastError());
    for(int row=0;row<x->ne[1];row+=2) {
        unsigned count=std::min<int>(2,static_cast<int>(x->ne[1])-row);
        const auto* input=static_cast<const float*>(x->data)+row*3072;auto* output=static_cast<float*>(dst->data)+row*1024;
        auto* trace=state.traces?state.traces+state.used:nullptr;bool modeled=state.mode>=2;
        std::vector<void*> args{&weights,&input,&output,&modeled,&trace};
        launch(1,count,row,reinterpret_cast<const void*>(m2_down<true>),args,stream);
    }
    return true;
}
extern "C" void hbflab_m2_step_end() {
    state.current();
    if(state.mode)for(int i=0;i<2;++i)need(state.expected[i]==1 && state.handled[i]==1,"M2 incomplete selected-range coverage");
    if(state.service) {
        state.service->synchronize();
        auto host=state.service->take_audit();need(host.size()==state.used,"M2 host audit missing or overflowed");
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
    state.current();json report={{"status","success"},{"mode",state.mode},{"matrix_precision","upstream per-token MMVF with GGML_PREC_F32 in all three modes; distinct from M0 default"},{"range_bytes",10240},{"steps",state.step+1},
        {"selected_requests",state.total},{"launches",state.launches},{"rows",state.rows},{"setup_ms",state.setup_ms},
        {"trace_capacity",trace_capacity},{"peak_trace_records",state.peak_traces},{"producer_slots",512},{"launch_slots",2},
        {"host_records",state.host_records},{"device_records",state.device_records},
        {"scope",{{"rms_norm",names[0]},{"f16_matmul",names[1]},{"f16_byte_offset",0},{"f16_bytes",6144}}},
        {"unselected_policy","native_unmodeled; other matrix rows use controlled arithmetic"},
        {"semantics","one exact 512 B norm / 128 B matrix span per cooperating warp, actual typed loads after completion; not a cache-miss trace"}};
    if(state.service) {
        state.service->finish();const auto s=state.service->statistics();const auto b=s.backend;const auto d=b.device;
        need(state.service->drained() && s.submitted==state.total && s.completed==state.total && !s.audit_dropped && state.service->take_audit().empty(),"M2 accounting/drain failure");
        report["runtime"]={{"submitted",s.submitted},{"completed",s.completed},{"grants",s.grants},
            {"launches_submitted",s.launches_submitted},{"launches_completed",s.launches_completed},
            {"peak_inflight",s.peak_inflight},{"peak_active_launches",s.peak_active_launches},
            {"gpu_epoch_ns",s.gpu_epoch_ns},{"model_epoch_ps",s.model_epoch_ps},{"model_now_ps",s.model_now_ps},
            {"drained",true},{"audit_dropped",s.audit_dropped}};
        report["backend"]={{"requests",b.requests},{"sectors",b.sectors},{"backpressure",b.backpressure},
            {"peak_registered_globals",b.peak_registered_globals},{"senses",d.senses},{"cache_hits",d.cache_hits},
            {"programs",d.programs},{"erases",d.erases},{"accepted",d.accepted},{"delivered",d.delivered}};
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
    state.service.reset();if(state.traces){cudaFree(state.traces);state.traces=nullptr;}
}
