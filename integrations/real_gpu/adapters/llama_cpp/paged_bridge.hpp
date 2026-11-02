#pragma once
#include "instrumented_bridge.hpp"
#include "hbflab/live_runtime.hpp"
void m4_begin(int mode,hbflab::LiveTimingService*,const char* output);
void m4_step_begin(int step);
void m4_step_end();
void m4_finish();
void m4_abort();
bool m4_is_attention(const ggml_tensor*);
void m4_attention(ggml_tensor*,cudaStream_t);

inline hbf_eventlab::Geometry m4_geometry(){
    auto g=hbflab::BackendOptions{}.geometry;
    g.pages_per_block=4;g.blocks_per_bank=2048;
    return g;
}
