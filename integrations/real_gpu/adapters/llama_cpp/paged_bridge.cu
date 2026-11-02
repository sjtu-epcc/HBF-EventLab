#include "paged_bridge.hpp"
#include "hbflab/capacity_gpu.hpp"
#include "hbflab/kv.hpp"
#include <cuda_fp16.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <cstdlib>
using namespace hbflab;
using json=nlohmann::ordered_json;
namespace {
void need(bool b,const char* m){if(!b)throw std::runtime_error(m);}
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
// No timing model in bypass; identical real page/lease/attention data path.
struct Bypass:CapacityMedia {
    std::uint64_t reserve_media(std::uint64_t) override{return 0;}
    ReadResult read_media(std::uint64_t,std::uint32_t n) override{return {Status::Ok,0,0,n/64};}
    WriteResult write_media(std::uint64_t,const std::array<std::uint64_t,64>&) override{return {Status::Ok,0,0,0};}
    BackendStats media_statistics() override{return {};}
    std::uint64_t media_time_ps() override{return 0;}
    void abort_media() override{}
};
CapacityOptions capacity_options(){CapacityOptions o;o.logical_pages=28*512;o.frames=4;o.backend.geometry=m4_geometry();return o;}
struct StorageConfig {
    CapacityOptions options=capacity_options();bool active=false,prefetch=false;double gpu_watts=0;
    json record;
    StorageConfig(const std::filesystem::path& output,bool modeled) {
        const char* path=std::getenv("HBFLAB_STORAGE_CONFIG");if(!path || !*path)return;
        std::ifstream input(std::filesystem::u8path(path));need(bool(input),"storage config open failed");json config;input>>config;
        need(config.is_object() && config.value("version",0)==1,"storage config version");
        for(const auto& item:config.items())need(item.key()=="version" || item.key()=="file_backing" || item.key()=="prefetch" || item.key()=="gpu_watts" || item.key()=="environment","unknown storage config field");
        active=true;prefetch=config.value("prefetch",false);gpu_watts=config.value("gpu_watts",0.0);need(std::isfinite(gpu_watts)&&gpu_watts>=0,"invalid supplied GPU watts");
        if(config.value("file_backing",false)){options.backing_mode=BackingMode::CreateFile;options.backing_path=output/"capacity.pages";}
        options.prefetch_evict_clean=prefetch;
        auto& e=options.environment;
        if(config.contains("environment")) {
            const auto& env=config["environment"];need(env.is_object(),"environment config object required");
            for(const auto& item:env.items())need(item.key()=="enabled" || item.key()=="ambient_c" || item.key()=="initial_c" || item.key()=="resistance_k_per_w" || item.key()=="capacitance_j_per_k" || item.key()=="gpu_coupling" || item.key()=="idle_w" || item.key()=="read_j_per_byte" || item.key()=="program_j_per_byte" || item.key()=="reference_c" || item.key()=="retention_seconds" || item.key()=="half_life_degrees" || item.key()=="refresh_fraction" || item.key()=="refresh_budget","unknown environment field");
            e.enabled=env.value("enabled",false);
#define HBFLAB_ENV_VALUE(name) e.name=env.value(#name,e.name)
            HBFLAB_ENV_VALUE(ambient_c);HBFLAB_ENV_VALUE(initial_c);HBFLAB_ENV_VALUE(resistance_k_per_w);HBFLAB_ENV_VALUE(capacitance_j_per_k);
            HBFLAB_ENV_VALUE(gpu_coupling);HBFLAB_ENV_VALUE(idle_w);HBFLAB_ENV_VALUE(read_j_per_byte);HBFLAB_ENV_VALUE(program_j_per_byte);
            HBFLAB_ENV_VALUE(reference_c);HBFLAB_ENV_VALUE(retention_seconds);HBFLAB_ENV_VALUE(half_life_degrees);HBFLAB_ENV_VALUE(refresh_fraction);HBFLAB_ENV_VALUE(refresh_budget);
#undef HBFLAB_ENV_VALUE
        }
        EnvironmentModel validate(e); // Reject invalid profiles even in bypass.
        record={{"version",1},{"requested",config},{"file_backing",options.backing_mode!=BackingMode::Memory},{"prefetch",prefetch},
            {"gpu_watts",gpu_watts},{"power_source","explicit configuration; not measured telemetry"},{"calibrated",false},
            {"environment_active",modeled&&e.enabled},{"sampling","model media completion and layer boundary"},
            {"environment",{{"enabled",e.enabled},{"ambient_c",e.ambient_c},{"initial_c",e.initial_c},{"resistance_k_per_w",e.resistance_k_per_w},
              {"capacitance_j_per_k",e.capacitance_j_per_k},{"gpu_coupling",e.gpu_coupling},{"idle_w",e.idle_w},{"read_j_per_byte",e.read_j_per_byte},
              {"program_j_per_byte",e.program_j_per_byte},{"reference_c",e.reference_c},{"retention_seconds",e.retention_seconds},
              {"half_life_degrees",e.half_life_degrees},{"refresh_fraction",e.refresh_fraction},{"refresh_budget",e.refresh_budget}}}};
        e.enabled=e.enabled&&modeled;
        std::ofstream saved(output/"storage-config.json");saved<<record.dump(2)<<'\n';saved.close();need(bool(saved),"storage config write failed");
    }
};
KvOptions kv_options(){KvOptions o;o.layers=28;o.heads=8;o.head_dimension=128;o.element_bytes=2;o.tokens_per_block=2;o.max_tokens=512;o.max_sequences=1;return o;}
__global__ void pack(const float* k,const float* v,half* hk,half* hv,unsigned n){unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i<n){hk[i]=__float2half_rn(k[i]);hv[i]=__float2half_rn(v[i]);}}
__global__ void init_attention(float* out,float* ml,unsigned n){unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i<n*128)out[i]=0;if(i<n){ml[i*2]=-INFINITY;ml[i*2+1]=0;}}
// One CTA per query/head. Each leased K/V page contains two complete tokens.
// Online softmax carries only output accumulators and max/denominator across pages.
__global__ void page_attention(const float* q,const half* k,const half* v,float* out,float* ml,
                               unsigned queries,unsigned past,unsigned first,unsigned count,unsigned offset) {
    const unsigned h=blockIdx.x%16,r=blockIdx.x/16,d=threadIdx.x,idx=r*16+h;
    __shared__ float partial[4],a,b;
    float acc=out[idx*128+d],m=ml[idx*2],l=ml[idx*2+1];
    for(unsigned j=0;j<count;++j) {
        if(first+j>past+r)break;
        float dot=q[idx*128+d]*__half2float(k[(offset+j)*1024+(h/2)*128+d]);
        for(int s=16;s;s/=2)dot+=__shfl_down_sync(0xffffffff,dot,s);
        if((d&31)==0)partial[d/32]=dot;
        __syncthreads();
        if(d==0){float score=((partial[0]+partial[1])+partial[2])+partial[3];score*=0.08838834764831845f;
            const float next=fmaxf(m,score);a=expf(m-next);b=expf(score-next);ml[idx*2]=next;ml[idx*2+1]=l*a+b;}
        __syncthreads();m=ml[idx*2];l=ml[idx*2+1];acc=acc*a+b*__half2float(v[(offset+j)*1024+(h/2)*128+d]);
        __syncthreads();
    }
    out[idx*128+d]=acc;
}
__global__ void normalize(float* out,const float* ml,unsigned n){unsigned i=blockIdx.x*blockDim.x+threadIdx.x;if(i<n)out[i]/=ml[(i/128)*2+1];}
struct State {
    Bypass bypass;LiveTimingService* service=nullptr;
    std::unique_ptr<CapacityGpu> gpu;std::unique_ptr<KvCache> kv;KvSequence sequence{};KvAppend tx{};
    std::unique_ptr<StorageConfig> storage;
    half* packed=nullptr;float* ml=nullptr;
    std::vector<half> keys,values;std::filesystem::path output;std::ofstream calls,media;
    unsigned step=0,layer=0,batch=0,past=0;bool active=false;std::uint64_t page_pairs=0,appended=0,media_count=0,copy_bytes=0;
    ~State(){kv.reset();gpu.reset();if(packed)cudaFree(packed);if(ml)cudaFree(ml);}
    void drain(){if(service)for(const auto& t:service->take_media_audit()){
        media<<json({{"step",step},{"layer",layer},{"ticket",t.ticket},{"dispatch",t.dispatch},{"producer",t.producer},
          {"write",t.write},{"logical",t.logical},{"bytes",t.bytes},{"floor_ns",t.floor_ns},{"arrival_ns",t.arrival_ns},
          {"arrival_ps",t.arrival_ps},{"end_ps",t.end_ps},{"replayed_pages",t.replayed_pages},
          {"ready_wall_ns",t.ready_wall_ns},{"completion_wall_ns",t.completion_wall_ns},{"resumed_wall_ns",t.resumed_wall_ns},{"delay_ns",t.delay_ns}}).dump()<<'\n';++media_count;}
        need(bool(media),"M4 media audit failed");}
};
std::unique_ptr<State> state;
}
bool m4_is_attention(const ggml_tensor* t){int tag=0;std::memcpy(&tag,reinterpret_cast<const char*>(t->op_params)+60,4);return t->op==GGML_OP_CUSTOM && tag==0x4d34564b;}
void m4_begin(int mode,LiveTimingService* service,const char* output){
    if(mode==0)return;need(!state,"M4 already initialized");state=std::make_unique<State>();auto& s=*state;s.service=service;s.output=output;
    s.calls.open(s.output/"kv-attention.jsonl");s.media.open(s.output/"kv-media.jsonl");need(s.calls && s.media,"M4 audit open failed");
    s.storage=std::make_unique<StorageConfig>(s.output,service!=nullptr);
    s.gpu=std::make_unique<CapacityGpu>(s.storage->options,service?static_cast<CapacityMedia*>(service):&s.bypass);
    s.kv=std::make_unique<KvCache>(s.gpu->store(),kv_options());s.sequence=s.kv->create();
    ck(cudaMalloc(reinterpret_cast<void**>(&s.packed),2*256*1024*sizeof(half)));ck(cudaMalloc(reinterpret_cast<void**>(&s.ml),256*16*2*sizeof(float)));
}
void m4_step_begin(int step){if(!state)return;auto& s=*state;need(!s.active && step==s.step,"M4 step order");s.active=true;s.layer=0;s.batch=0;s.past=static_cast<unsigned>(s.kv->length(s.sequence));}
void m4_attention(ggml_tensor* dst,cudaStream_t stream){
    need(state && state->active && m4_is_attention(dst),"M4 attention lifecycle");auto& s=*state;
    int layer;std::memcpy(&layer,reinterpret_cast<const char*>(dst->op_params)+56,4);
    auto q=dst->src[0],k=dst->src[1],v=dst->src[2];const unsigned n=static_cast<unsigned>(q->ne[2]);
    need(layer==s.layer && layer<28 && n>=1 && n<=256 && q->type==GGML_TYPE_F32 && k->type==GGML_TYPE_F32 && v->type==GGML_TYPE_F32 &&
      ggml_is_contiguous(q)&&ggml_is_contiguous(k)&&ggml_is_contiguous(v) && q->ne[0]==128 && q->ne[1]==16 && q->ne[3]==1 &&
      ggml_nelements(k)==n*1024 && ggml_nelements(v)==n*1024 && ggml_nelements(dst)==n*2048,"M4 unsupported attention shape/order");
    need(dst->src[3] && dst->src[3]->type==GGML_TYPE_I32 && ggml_nelements(dst->src[3])==n,"M4 position tensor required");
    if(!layer){std::vector<int> positions(n);ck(cudaMemcpyAsync(positions.data(),dst->src[3]->data,n*4,cudaMemcpyDeviceToHost,stream));ck(cudaStreamSynchronize(stream));
        for(unsigned i=0;i<n;++i)need(positions[i]==s.past+i,"M4 requires contiguous append positions");}
    if(!layer){s.batch=n;s.tx=s.kv->begin_append(s.sequence,n);}else need(n==s.batch,"M4 inconsistent layer batch");
    pack<<<(n*1024+255)/256,256,0,stream>>>(static_cast<const float*>(k->data),static_cast<const float*>(v->data),s.packed,s.packed+256*1024,n*1024);ck(cudaGetLastError());
    s.keys.resize(n*1024);s.values.resize(n*1024);
    ck(cudaMemcpyAsync(s.keys.data(),s.packed,n*2048,cudaMemcpyDeviceToHost,stream));ck(cudaMemcpyAsync(s.values.data(),s.packed+256*1024,n*2048,cudaMemcpyDeviceToHost,stream));ck(cudaStreamSynchronize(stream));
    s.copy_bytes+=n*4096;s.kv->write_layer(s.tx,layer,s.keys.data(),s.values.data(),n*2048);s.drain();
    auto out=static_cast<float*>(dst->data);init_attention<<<(n*2048+255)/256,256,0,stream>>>(out,s.ml,n*16);ck(cudaGetLastError());
    const auto before=s.gpu->store().statistics();unsigned pairs=0;
    for(unsigned first=0;first<s.past+n;first+=2){
        auto ks=s.kv->locate_layer(s.tx,layer,first,KvKind::Key),vs=s.kv->locate_layer(s.tx,layer,first,KvKind::Value);
        if(s.storage->prefetch){s.gpu->prefetch(ks.allocation,ks.offset/4096,1);s.gpu->prefetch(vs.allocation,vs.offset/4096,1);}
        auto kl=s.gpu->acquire(ks.allocation,ks.offset/4096,PageAccess::Read,stream);
        auto vl=s.gpu->acquire(vs.allocation,vs.offset/4096,PageAccess::Read,stream);
        page_attention<<<n*16,128,0,stream>>>(static_cast<const float*>(q->data),static_cast<const half*>(s.gpu->data(kl)),static_cast<const half*>(s.gpu->data(vl)),out,s.ml,n,s.past,first,std::min(2u,s.past+n-first),ks.offset%4096/2048);ck(cudaGetLastError());
        s.gpu->finish(vl);s.gpu->finish(kl);++pairs;s.drain();
    }
    normalize<<<(n*2048+255)/256,256,0,stream>>>(out,s.ml,n*2048);ck(cudaGetLastError());
    const auto after=s.gpu->store().statistics();
    s.calls<<json({{"step",s.step},{"layer",layer},{"past",s.past},{"batch",n},{"visible",s.past+n},{"committed",s.kv->length(s.sequence)},
      {"page_pairs",pairs},{"read_faults",after.faults-before.faults},{"frame_bytes",s.gpu->device_bytes()}}).dump()<<'\n';need(bool(s.calls),"M4 attention audit failed");
    s.page_pairs+=pairs;s.appended+=n;++s.layer;
    if(s.storage->options.environment.enabled){s.gpu->maintain(s.storage->gpu_watts);s.drain();}
}
void m4_step_end(){if(!state)return;auto& s=*state;need(s.active && s.layer==28,"M4 missing layer attention");s.kv->commit(s.tx);need(s.kv->length(s.sequence)==s.past+s.batch,"M4 publish length");s.drain();s.active=false;++s.step;}
void m4_finish(){if(!state)return;auto& s=*state;need(!s.active,"M4 unfinished batch");s.kv->flush(s.sequence);s.drain();
    if(s.storage->active){s.gpu->flush();s.drain();}
    const auto c=s.gpu->store().statistics();const auto g=s.gpu->statistics();need(!c.pins&&!c.dirty_frames&&!g.active_leases,"M4 leases/dirty data not drained");
    std::ofstream f(s.output/"kv-summary.json");f<<json({{"steps",s.step},{"length",s.kv->length(s.sequence)},{"layers",28},{"appended_layer_tokens",s.appended},
      {"page_pairs",s.page_pairs},{"frame_bytes",g.device_bytes},{"logical_pages",14336},{"pages_per_block",m4_geometry().pages_per_block},{"blocks_per_bank",m4_geometry().blocks_per_bank},{"allocated_pages",c.allocated_pages},{"faults",c.faults},{"hits",c.hits},
      {"evictions",c.evictions},{"writebacks",c.writebacks},{"dirty_frames",c.dirty_frames},{"pins",c.pins},{"media_records",s.media_count},
      {"kv_commit_d2h_bytes",s.copy_bytes},{"h2d_bytes",g.h2d_bytes},{"native_gpu_kv_bytes",0}}).dump(2)<<'\n';f.close();need(bool(f),"M4 summary failed");
    if(s.storage->active){std::ofstream storage(s.output/"storage-summary.json");storage<<json({
        {"file_read_bytes",c.file_read_bytes},{"file_write_bytes",c.file_write_bytes},{"checkpoints",c.checkpoints},
        {"prefetch_fills",c.prefetch_fills},{"prefetch_useful",c.prefetch_useful},{"prefetch_wasted",c.prefetch_wasted},{"prefetch_skipped",c.prefetch_skipped},
        {"prefetch_uploads",g.prefetch_uploads},
        {"refreshes",c.refreshes},{"refresh_read_bytes",c.refresh_read_bytes},{"refresh_pending",c.refresh_pending},{"refresh_blocked",c.refresh_blocked},
        {"retention_expirations",c.retention_expirations},{"temperature_c",c.environment.temperature_c},{"peak_c",c.environment.peak_c},
        {"media_energy_j",c.environment.media_energy_j},{"exposure",c.environment.exposure},{"samples",c.environment.samples},
        {"calibrated",false},{"model_resume_supported",false}}).dump(2)<<'\n';storage.close();need(bool(storage),"storage summary failed");}
    s.kv->release(s.sequence);need(!s.gpu->store().statistics().allocated_pages,"M4 allocation leak");
    if(s.storage->active)s.gpu->flush(); // Persist the final empty allocation catalog.
    state.reset();}
void m4_abort(){state.reset();}
