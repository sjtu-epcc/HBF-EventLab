#include "integrated_profile.hpp"
#include "hbflab/module.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cctype>
namespace {
M6Profile profile;
nlohmann::json document;
bool loaded=false;
void need(bool b,const char* text){if(!b)throw std::runtime_error(text);}
}
const M6Profile& integrated_profile(){need(loaded,"M6 profile not loaded");return profile;}
void m6_load_profile(const char* path,const char* output){
    need(!loaded,"M6 profile already loaded");
    std::ifstream file(std::filesystem::u8path(path),std::ios::binary);need(bool(file),"M6 profile open failed");
    std::string text((std::istreambuf_iterator<char>(file)),{});document=nlohmann::json::parse(text);const auto& j=document;auto& p=profile;
    need(j.at("schema")==1 && j.at("framework_commit")=="b3fa69955ea81c3b60e77e3c1e2224c4a826ffb9","M6 profile version/commit");
    p.architecture=j.at("architecture");need(p.architecture=="qwen3"||p.architecture=="llama","M6 unsupported architecture");
    p.layers=j.at("layers");p.hidden=j.at("hidden");p.ffn=j.at("ffn");p.q_heads=j.at("q_heads");p.kv_heads=j.at("kv_heads");p.head_dim=j.at("head_dim");
    p.context=j.at("context");p.batch=j.at("batch");p.ubatch=j.at("ubatch");p.block_tokens=j.at("block_tokens");p.frames=j.at("frames");p.logical_pages=j.at("logical_pages");
    p.pages_per_block=j.at("pages_per_block");p.blocks_per_bank=j.at("blocks_per_bank");p.norm_name=j.at("norm_name");p.down_name=j.at("down_name");
    const auto& b=j.at("budgets");p.trace_records=b.at("trace_records");p.audit_records=b.at("audit_records");p.max_events=b.at("max_events");p.gpu_required=b.at("gpu_required_bytes");p.host_required=b.at("host_required_bytes");
    need(p.layers>1&&p.layers<=256&&p.hidden>=64&&p.hidden<=8192&&p.hidden%32==0&&p.ffn>0&&p.ffn<=32768&&p.ffn%512==0,"M6 unsupported model dimensions");
    need(p.kv_heads>0&&p.q_heads<=64&&p.q_heads%p.kv_heads==0&&(p.head_dim==32||p.head_dim==64||p.head_dim==128||p.head_dim==256)&&p.kv_elements()*2<=8192,"M6 unsupported head dimensions");
    need(p.context>=32&&p.context<=4096&&p.batch==256&&p.ubatch>0&&p.ubatch<=p.batch&&p.block_tokens>0&&p.block_tokens<=256&&j.at("sequences")==1,"M6 unsupported execution dimensions");
    const auto row=p.kv_elements()*2;unsigned minimum=0;
    for(unsigned t=0;t<p.block_tokens;++t)minimum=std::max(minimum,2*((t*row%4096+row+4095)/4096));
    const auto logical=p.layers*2*((p.context+p.block_tokens-1)/p.block_tokens)*((row*p.block_tokens+4095)/4096);
    need(p.frames>=minimum&&p.frames<=64&&p.logical_pages==logical&&p.pages_per_block==4&&p.blocks_per_bank>=2048,"M6 insufficient page/frame budget");
    need(p.trace_records>=p.batch*p.ffn/64&&p.audit_records>=p.batch*(p.hidden+p.ffn/64)&&p.max_events>0,"M6 insufficient audit/event budget");
    need(j.at("rope").at("scaling")=="none"&&j.at("kv_dtype")=="F16","M6 unsupported rope/dtype");
    p.hash=hbflab::sha256(text);loaded=true;
    _putenv_s("HBFLAB_M6_NORM",p.norm_name.c_str());
    std::ofstream saved(std::filesystem::u8path(output)/"profile.json",std::ios::binary);saved<<text;saved.close();need(bool(saved),"M6 profile write failed");
}
void m6_check_tensor(const ggml_tensor* t,const char* name){
    need(t&&std::strcmp(t->name,name)==0,"M6 missing model tensor");
    for(const auto& expected:document.at("tensors"))if(expected.at("name")==name){
        std::string dtype=expected.at("dtype");std::transform(dtype.begin(),dtype.end(),dtype.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        need(dtype==ggml_type_name(t->type)&&expected.at("bytes").get<std::size_t>()==ggml_nbytes(t),"M6 actual tensor dtype/size mismatch");
        const auto& shape=expected.at("shape");for(unsigned i=0;i<4;++i)need(t->ne[i]==(i<shape.size()?shape[i].get<std::int64_t>():1),"M6 actual tensor shape mismatch");
        return;
    }
    throw std::runtime_error("M6 tensor absent from profile");
}
