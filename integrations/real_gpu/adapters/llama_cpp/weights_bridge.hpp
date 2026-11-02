#pragma once
#include "hbflab/live_runtime.hpp"
#include "ggml.h"
void m5_begin(hbflab::LiveTimingService*,int,const char*);
void m5_mul(const float*,const float*,float*,unsigned,cudaStream_t);
void m5_step();
std::uint64_t m5_step_requests();
std::uint64_t m5_total_requests();
void m5_finish();
void m5_abort();
bool hbflab_m5_mul(ggml_tensor*,cudaStream_t);
