#pragma once
#include "ggml.h"
#include <cuda_runtime_api.h>
#ifdef _WIN32
#ifdef HBFLAB_M1_BUILD
#define HBFLAB_M1_API __declspec(dllexport)
#else
#define HBFLAB_M1_API __declspec(dllimport)
#endif
#else
#define HBFLAB_M1_API
#endif
extern "C" {
HBFLAB_M1_API void hbflab_m1_configure(const char* mode,const char* output);
HBFLAB_M1_API void hbflab_m1_step_begin(int step);
HBFLAB_M1_API void hbflab_m1_graph_node(const ggml_tensor*);
HBFLAB_M1_API void hbflab_m1_step_end();
HBFLAB_M1_API void hbflab_m1_finish();
HBFLAB_M1_API void hbflab_m1_abort();
}
bool hbflab_m1_rms_norm(const ggml_tensor* x,const ggml_tensor* mul,const ggml_tensor* add,
                      float* dst,int ncols,int nrows,int nchannels,int nsamples,float eps,cudaStream_t stream);
