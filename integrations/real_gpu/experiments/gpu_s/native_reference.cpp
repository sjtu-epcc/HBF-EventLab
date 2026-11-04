// S6 low-instrumentation control; derived from the frozen S5 native.cpp.
#include "llama.h"
#include "ggml-backend.h"
#include "hbflab/platform.hpp"
#include <cuda_runtime_api.h>
#include "cupti_compat.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;
static fs::path directory;
static std::ofstream events, snapshots, kernels;
#ifdef GPU_S_TRANSFER_TRACE
static std::ofstream transfers;
#ifdef GPU_S_SEGMENTS
static std::ofstream apis;
#endif
#endif
static std::mutex kernel_mutex;
static std::atomic<int> step{-1};
static std::atomic<unsigned> lost{0}, kernel_errors{0};
static std::uint64_t event_id=0, node_id=0, callback_id=0;
static std::string callback_error;
static std::map<const ggml_tensor*,json> pending;
static bool verify_mode=true;
static int valid_positions=0;
static bool stop_before_selected=false, intentional_stop=false;
static void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static std::uint64_t stamp(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
#ifdef GPU_S_JOINT
#include "hbf_gate.hpp"
static std::unique_ptr<S3Gate> gate;
#endif
#ifdef GPU_S_SEGMENTS
#include "segments.hpp"
#endif
static void emit(json row){row["event_id"]=++event_id;row["request_id"]=1;row["step"]=step.load();row["timestamp_ns"]=stamp();events<<row.dump()<<'\n';need(bool(events),"Event write failed");}
static json info(const ggml_tensor* t){return {{"name",t->name},{"dtype",ggml_type_name(t->type)},{"shape",{t->ne[0],t->ne[1],t->ne[2],t->ne[3]}},{"strides",{t->nb[0],t->nb[1],t->nb[2],t->nb[3]}},{"bytes",ggml_nbytes(t)},{"address",reinterpret_cast<std::uintptr_t>(t->data)}};}
static bool snapshot_name(const std::string& name){return name.rfind("l_out-",0)==0 || name.rfind("Qcur-",0)==0 || name.rfind("Kcur-",0)==0 || name.rfind("Vcur-",0)==0 || name=="ffn_inp-10" || name=="ffn_norm-10" || name=="result_norm";}
static bool observe(ggml_tensor* t,bool ask,void*){
    if(step<0)return false;
    if(!verify_mode){
        if(ask && t->op==GGML_OP_MUL_MAT)ggml_mul_mat_set_prec(t,GGML_PREC_F32);
        return true; // Preserve one-node scheduler dispatch, without per-node export.
    }
    try{
        if(ask){
#ifdef GPU_S_SEGMENTS
            segment_end("gap",node_id+1);
#endif
            // The cuBLAS environment setting does not control MMVF accumulation.
            // Use the public precision hint for every matrix consumer as well.
            if(t->op==GGML_OP_MUL_MAT)ggml_mul_mat_set_prec(t,GGML_PREC_F32);
            json row={{"type","node_before"},{"node_id",++node_id},{"op",ggml_op_name(t->op)},{"tensor",info(t)},{"sources",json::array()},{"weight_accesses",json::array()}};
            for(auto* src:t->src)if(src){row["sources"].push_back(info(src));const std::string name=src->name;
                if(name.find(".weight")!=std::string::npos)row["weight_accesses"].push_back({{"callback_id",++callback_id},{"source",info(src)},{"source_offset",0},{"source_bytes",ggml_nbytes(src)}});
            }
            // Returning true for EVERY node makes the scheduler dispatch a one-node
            // graph, synchronize it, then invoke this callback with ask=false.
            pending[t]=row;emit(row);
#ifdef GPU_S_JOINT
            if(gate)gate->before(row,step.load());
#endif
            if(stop_before_selected && t->op==GGML_OP_MUL && t->src[1] && std::string(t->src[1]->name)=="blk.10.ffn_norm.weight"){
                emit({{"type","consumer_stop_before_dispatch"},{"node_id",row["node_id"]}});cuptiActivityFlushAll(0);intentional_stop=true;throw std::runtime_error("Intentional pre-dispatch stop fixture");
            }
#ifdef GPU_S_SEGMENTS
            segment_begin();
#endif
            return true;
        }
        auto row=pending.at(t);
#ifdef GPU_S_SEGMENTS
        segment_end("node",row["node_id"]);
#endif
        emit({{"type","node_after"},{"node_id",row["node_id"]},{"op",row["op"]},{"tensor_name",t->name}});
        const bool kv_copy=t->op==GGML_OP_CPY && t->src[0] && (std::string(t->src[0]->name).rfind("Kcur-",0)==0 || std::string(t->src[0]->name).rfind("Vcur-",0)==0);
        const std::string cache_name=t->src[2]?t->src[2]->name:"";
        const bool kv_store=t->op==GGML_OP_SET_ROWS && (cache_name.rfind("cache_k_l",0)==0 || cache_name.rfind("cache_v_l",0)==0);
        if(verify_mode && (snapshot_name(t->name) || kv_copy || kv_store) && (t->type==GGML_TYPE_F32 || t->type==GGML_TYPE_F16)){
#ifdef GPU_S_SEGMENTS
            segment_begin();
#endif
            const auto file="snapshot-"+std::to_string(row["node_id"].get<std::uint64_t>())+".bin";
            std::vector<unsigned char> bytes(ggml_nbytes(t));ggml_backend_tensor_get(t,bytes.data(),0,bytes.size());
            std::ofstream f(directory/file,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());need(bool(f),"Snapshot write failed");
            auto meta=info(t);if(kv_copy)meta["semantic_name"]=std::string("kv_copy:")+t->src[0]->name;if(kv_store){meta["kv_kind"]=cache_name[6]=='k'?"K":"V";meta["layer"]=std::stoi(cache_name.substr(9));meta["valid_positions"]=valid_positions;}
            meta["file"]=file;meta["step"]=step.load();meta["node_id"]=row["node_id"];snapshots<<meta.dump()<<'\n';
#ifdef GPU_S_SEGMENTS
            segment_end("verification",row["node_id"]);
#endif
        }
#ifdef GPU_S_SEGMENTS
        segment_begin();
#endif
        pending.erase(t);return true;
    }catch(const std::exception& e){
        callback_error=e.what();
#ifdef GPU_S_JOINT
        throw; // No callback failure may allow a dependent consumer to dispatch.
#else
        if(intentional_stop)throw;return false;
#endif
    }
}
static void CUPTIAPI request_buffer(std::uint8_t** data,std::size_t* size,std::size_t* records){*size=8*1024*1024;*records=0;*data=static_cast<std::uint8_t*>(hbflab::platform::aligned_allocate(*size,8));if(!*data){*size=0;++kernel_errors;}}
static void CUPTIAPI complete_buffer(CUcontext ctx,std::uint32_t stream,std::uint8_t* data,std::size_t,std::size_t valid){
    std::lock_guard<std::mutex> guard(kernel_mutex);
    try{CUpti_Activity* a=nullptr;CUptiResult r;
        while((r=cuptiActivityGetNextRecord(data,valid,&a))==CUPTI_SUCCESS){
#ifdef GPU_S_SEGMENTS
            if(a->kind==CUPTI_ACTIVITY_KIND_RUNTIME||a->kind==CUPTI_ACTIVITY_KIND_DRIVER){
                const auto* api=reinterpret_cast<const CUpti_ActivityAPI*>(a);
                apis<<json({{"domain",a->kind==CUPTI_ACTIVITY_KIND_RUNTIME?"runtime":"driver"},{"cbid",api->cbid},
                    {"correlation",api->correlationId},{"start_ns",api->start},{"end_ns",api->end},
                    {"return_value",api->returnValue},{"thread",api->threadId}}).dump()<<'\n';continue;
            }
            const auto aid=++activity_id;
#define ACTIVITY_ID {"activity_id",aid},
#else
#define ACTIVITY_ID
#endif
#ifdef GPU_S_TRANSFER_TRACE
            if(a->kind==CUPTI_ACTIVITY_KIND_MEMCPY){const auto* m=reinterpret_cast<const CUpti_ActivityMemcpy6*>(a);
                transfers<<json({ACTIVITY_ID {"kind","memcpy"},{"step",step.load()},{"start_ns",m->start},{"end_ns",m->end},{"stream",m->streamId},{"bytes",m->bytes},{"copy_kind",m->copyKind},{"correlation",m->correlationId}}).dump()<<'\n';continue;}
            if(a->kind==CUPTI_ACTIVITY_KIND_MEMSET){const auto* m=reinterpret_cast<const CUpti_ActivityMemset4*>(a);
                transfers<<json({ACTIVITY_ID {"kind","memset"},{"step",step.load()},{"start_ns",m->start},{"end_ns",m->end},{"stream",m->streamId},{"bytes",m->bytes},{"correlation",m->correlationId}}).dump()<<'\n';continue;}
#endif
            if(a->kind!=CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL)continue;
            const auto* k=reinterpret_cast<const GpuSKernelActivity*>(a);
            kernels<<json({ACTIVITY_ID {"step",step.load()},{"name",k->name},{"start_ns",k->start},{"end_ns",k->end},{"stream",k->streamId},{"correlation",k->correlationId}}).dump()<<'\n';}
        if(r!=CUPTI_ERROR_MAX_LIMIT_REACHED)++kernel_errors;
        std::size_t count=0;if(cuptiActivityGetNumDroppedRecords(ctx,stream,&count)!=CUPTI_SUCCESS)++kernel_errors;lost+=static_cast<unsigned>(count);
    }catch(...){++kernel_errors;}
    hbflab::platform::aligned_release(data);
}
static int run(int argc,char** argv){
    verify_cupti_abi();
    need(argc==7,"Usage: gpu_s_native model.gguf prompt.txt output count probe|verify|timing config.json");
    directory=argv[3];need(!fs::exists(directory),"Output already exists");fs::create_directories(directory);
    const int count=std::stoi(argv[4]);const std::string mode=argv[5];const bool probe=mode=="probe";verify_mode=mode=="verify";
    need(probe || verify_mode || mode=="timing","Invalid mode");need(count>=0 && count<=8,"Count outside 0..8");
    std::ifstream cf(argv[6]);json config;cf>>config;
#ifdef GPU_S_SEGMENTS
    apis.open(directory/"apis.jsonl");need(bool(apis),"API trace open failed");
    segments.open(directory/"segments.jsonl");need(bool(segments),"Segment trace open failed");
    fixed_segment_ps=config.value("fixed_segment_ps",std::uint64_t{1000});
    need(fixed_segment_ps==0||fixed_segment_ps==1000,"S5 segment duration must be 0 or 1000ps");
#endif
    stop_before_selected=config.value("stop_before_selected",false);
#ifdef GPU_S_JOINT
    // Open records before initialization so failure-only cases retain evidence.
    events.open(directory/"events.jsonl");snapshots.open(directory/"snapshots.jsonl");kernels.open(directory/"kernels.jsonl");
#ifdef GPU_S_TRANSFER_TRACE
    transfers.open(directory/"transfers.jsonl");need(bool(transfers),"Transfer trace open failed");
#endif
    if(config.contains("hbf")&&config.at("hbf").value("enabled",false)) {
        gate=std::make_unique<S3Gate>(directory,config.at("hbf"),stamp);gate->initialize();
    }
#endif
    hbflab::platform::set_environment("HBFLAB_M4_PAGED","0");
    llama_backend_init();auto mp=llama_model_default_params();mp.vocab_only=probe;mp.n_gpu_layers=probe?0:999;mp.split_mode=LLAMA_SPLIT_MODE_NONE;
    std::unique_ptr<llama_model,decltype(&llama_model_free)> model(llama_model_load_from_file(argv[1],mp),llama_model_free);need(bool(model),"Cannot load model");
    std::ifstream prompt_file(argv[2],std::ios::binary);need(bool(prompt_file),"Cannot read prompt");const std::string prompt((std::istreambuf_iterator<char>(prompt_file)),{});
    const auto* vocab=llama_model_get_vocab(model.get());const int length=-llama_tokenize(vocab,prompt.data(),static_cast<int>(prompt.size()),nullptr,0,true,true);need(length>0,"Empty tokenized input");
    std::vector<llama_token> ids(length);need(llama_tokenize(vocab,prompt.data(),static_cast<int>(prompt.size()),ids.data(),length,true,true)==length,"Tokenization mismatch");
    json summary={{"prompt_ids",ids},{"prompt",prompt},{"requested_outputs",count},{"mode",mode},{"native_kv",true},{"hbf_submitted",0},{"matrix_precision","GGML_PREC_F32 on every MUL_MAT; cuBLAS f32; TF32 disabled"}};
    int devices=0,driver=0,runtime=0;cudaDeviceProp prop{};std::size_t available=0,total=0;
    need(cudaGetDeviceCount(&devices)==cudaSuccess && devices==1,"Exactly one visible GPU required");need(cudaGetDeviceProperties(&prop,0)==cudaSuccess,"GPU properties failed");need(cudaMemGetInfo(&available,&total)==cudaSuccess,"GPU memory query failed");cudaDriverGetVersion(&driver);cudaRuntimeGetVersion(&runtime);
    summary["environment"]={{"gpu",prop.name},{"compute_capability",{prop.major,prop.minor}},{"cuda_driver_api",driver},{"cuda_runtime",runtime},{"gpu_free_bytes",available},{"gpu_total_bytes",total},{"host_available_bytes",hbflab::platform::available_host_memory()}};
    if(probe){std::ofstream(directory/"summary.json")<<summary.dump(2);return 0;}
    need(length<=config.at("batch").get<int>() && length+count<=config.at("context").get<int>(),"Context budget exceeded");
#ifndef GPU_S_JOINT
    events.open(directory/"events.jsonl");snapshots.open(directory/"snapshots.jsonl");kernels.open(directory/"kernels.jsonl");
#endif
    need(cuptiActivityRegisterCallbacks(request_buffer,complete_buffer)==CUPTI_SUCCESS,"CUPTI callbacks failed");
#ifdef GPU_S_TRANSFER_TRACE
    need(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMCPY)==CUPTI_SUCCESS,"CUPTI memcpy activity failed");
    need(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_MEMSET)==CUPTI_SUCCESS,"CUPTI memset activity failed");
#endif
#ifdef GPU_S_SEGMENTS
    need(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_RUNTIME)==CUPTI_SUCCESS,"CUPTI runtime activity failed");
    need(cuptiActivityEnable(CUPTI_ACTIVITY_KIND_DRIVER)==CUPTI_SUCCESS,"CUPTI driver activity failed");
#endif
    auto cp=llama_context_default_params();cp.n_ctx=config.at("context");cp.n_batch=config.at("batch");cp.n_ubatch=config.at("ubatch");cp.n_seq_max=1;cp.n_threads=8;cp.n_threads_batch=8;cp.type_k=GGML_TYPE_F16;cp.type_v=GGML_TYPE_F16;cp.flash_attn_type=LLAMA_FLASH_ATTN_TYPE_DISABLED;cp.cb_eval=observe;
    std::unique_ptr<llama_context,decltype(&llama_free)> ctx(llama_init_from_model(model.get(),cp),llama_free);need(bool(ctx),"Context creation failed");
    std::unique_ptr<llama_batch_ext,decltype(&llama_batch_ext_free)> batch(llama_batch_ext_init(ctx.get()),llama_batch_ext_free);
    std::unique_ptr<llama_sampler,decltype(&llama_sampler_free)> sampler(llama_sampler_init_greedy(),llama_sampler_free);
    std::vector<llama_token> input=ids,generated,raw;int position=0;std::string reason="length";
#ifdef GPU_S_JOINT
    if(gate)gate->request_begin();
#endif
#ifdef GPU_S_SEGMENTS
    need(cudaDeviceSynchronize()==cudaSuccess,"Request synchronization failed");
    need(cuptiActivityFlushAll(0)==CUPTI_SUCCESS,"Initialization flush failed");
#endif
    emit({{"type","request_begin"}});std::ofstream logits(directory/"logits.f32",std::ios::binary);
    for(int i=0;i<count;++i){step=i;llama_batch_ext_clear(batch.get());const int first=position;
        for(auto token:input){const int index=llama_batch_ext_add_token(batch.get(),0,token);const llama_pos pos=position++;need(index>=0 && llama_batch_ext_set_pos(batch.get(),index,&pos),"Batch insertion failed");}
        need(llama_batch_ext_set_output_logits(batch.get(),static_cast<int>(input.size())-1,true),"Logit selection failed");
        valid_positions=position;emit({{"type","execution_begin"},{"position_begin",first},{"position_end",position},{"phase",i==0?"prefill":"decode"}});
#ifdef GPU_S_SEGMENTS
        segment_begin();
#endif
        need(llama_process(ctx.get(),LLAMA_PROCESS_TYPE_DECODE,batch.get())==0,"GPU inference failed");llama_synchronize(ctx.get());need(callback_error.empty(),callback_error.c_str());need(pending.empty(),"Unpaired callbacks");
#ifdef GPU_S_SEGMENTS
        segment_end("tail",node_id);
#endif
        emit({{"type","execution_end"},{"position_begin",first},{"position_end",position}});if(i==0)emit({{"type","prefill_finished"}});
        need(cuptiActivityFlushAll(0)==CUPTI_SUCCESS,"CUPTI flush failed");const float* values=llama_get_logits_ith(ctx.get(),-1);need(values,"Missing logits");
        const int nv=llama_vocab_n_tokens(vocab);for(int j=0;j<nv;++j)need(std::isfinite(values[j]),"Non-finite logits");
        const auto token=llama_sampler_sample(sampler.get(),ctx.get(),-1);raw.push_back(token);
#ifdef GPU_S_JOINT
        if(gate)gate->token(token,generated.size()+1,llama_vocab_is_eog(vocab,token));
#endif
        if(llama_vocab_is_eog(vocab,token)){reason="EOS";emit({{"type","eos"},{"token_id",token}});}
        else{generated.push_back(token);emit({{"type","token_emitted"},{"output_index",generated.size()},{"token_id",token}});}
        logits.write(reinterpret_cast<const char*>(values),nv*sizeof(float));if(reason=="EOS")break;input.assign(1,token);
    }
    emit({{"type","request_end"},{"reason",reason},{"actual_outputs",generated.size()}});need(cuptiActivityFlushAll(0)==CUPTI_SUCCESS,"Final CUPTI flush failed");
#ifdef GPU_S_TRANSFER_TRACE
    cuptiActivityDisable(CUPTI_ACTIVITY_KIND_MEMCPY);cuptiActivityDisable(CUPTI_ACTIVITY_KIND_MEMSET);transfers.flush();
#endif
#ifdef GPU_S_SEGMENTS
    cuptiActivityDisable(CUPTI_ACTIVITY_KIND_RUNTIME);cuptiActivityDisable(CUPTI_ACTIVITY_KIND_DRIVER);apis.flush();segments.flush();
#endif
    need(!lost && !kernel_errors,"Incomplete kernel trace");summary["tokens"]=generated;summary["raw_tokens"]=raw;summary["stop_reason"]=reason;summary["positions"]=position;summary["vocab_size"]=llama_vocab_n_tokens(vocab);summary["nodes"]=node_id;summary["callbacks"]=callback_id;summary["cupti_dropped"]=lost.load();summary["status"]="completed";
#ifdef GPU_S_JOINT
    if(gate) {
        const auto& s=gate->finish();summary["hbf_submitted"]=gate->submitted();
        need(s.at("status")=="Completed"&&s.at("workload_success")==true&&s.at("host_drained")==true&&s.at("device_drained")==true,"HBF final state not successful/drained");
    }
#endif
    std::ofstream(directory/"summary.json")<<summary.dump(2);return 0;
}
int main(int argc,char** argv){try{return run(argc,argv);}
#ifdef GPU_S_JOINT
    catch(const HbfDependencyError& e) {
        std::cerr<<e.what()<<'\n';cuptiActivityFlushAll(0);
        std::ofstream(directory/"failure.json")<<json({{"reason",e.what()},{"nodes_seen",node_id},{"cupti_dropped",lost.load()},{"kernel_errors",kernel_errors.load()},{"dependency_consumer_dispatched",false}}).dump(2);
        if(gate)gate->finish();return gate&&gate->initialization_failed()?87:88;
    }
#endif
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';
#ifdef GPU_S_JOINT
        if(gate)gate->finish();
#endif
        if(intentional_stop){std::ofstream(directory/"intentional-stop.json")<<json({{"expected_stop",true},{"reason",e.what()},{"cupti_dropped",lost.load()}}).dump(2);return 86;}return 1;}}
