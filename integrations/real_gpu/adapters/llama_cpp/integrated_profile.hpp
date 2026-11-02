#pragma once
#include "instrumented_bridge.hpp"
#include <string>
#include <cstdint>

struct M6Profile {
    unsigned layers=0,hidden=0,ffn=0,q_heads=0,kv_heads=0,head_dim=0;
    unsigned context=0,batch=0,ubatch=0,block_tokens=0,frames=0,logical_pages=0;
    unsigned pages_per_block=0,blocks_per_bank=0,trace_records=0,audit_records=0;
    std::uint64_t max_events=0,gpu_required=0,host_required=0;
    std::string architecture,norm_name,down_name,hash;
    unsigned kv_elements() const {return kv_heads*head_dim;}
    unsigned q_elements() const {return q_heads*head_dim;}
};
HBFLAB_M2_API const M6Profile& integrated_profile();
HBFLAB_M2_API void m6_load_profile(const char* path,const char* output);
HBFLAB_M2_API void m6_check_tensor(const ggml_tensor* tensor,const char* name);
