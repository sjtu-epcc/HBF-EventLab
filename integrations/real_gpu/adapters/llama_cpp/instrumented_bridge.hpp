#pragma once
#include "ggml.h"
#include <cuda_runtime_api.h>
#ifdef _WIN32
#ifdef HBFLAB_M2_BUILD
#define HBFLAB_M2_API __declspec(dllexport)
#else
#define HBFLAB_M2_API __declspec(dllimport)
#endif
#else
#define HBFLAB_M2_API
#endif
struct llama_model;
extern "C" const ggml_tensor* hbflab_m2_find_tensor(const llama_model*,const char*);
extern "C" {
HBFLAB_M2_API void hbflab_m2_bind(const ggml_tensor*,const ggml_tensor*);
HBFLAB_M2_API void hbflab_m2_configure(const char* mode,const char* output);
HBFLAB_M2_API void hbflab_m2_step_begin(int step);
HBFLAB_M2_API void hbflab_m2_graph_node(const ggml_tensor*);
HBFLAB_M2_API void hbflab_m2_step_end();
HBFLAB_M2_API void hbflab_m2_finish();
HBFLAB_M2_API void hbflab_m2_abort();
}
bool hbflab_m2_rms_norm(const ggml_tensor* x,const ggml_tensor* mul,const ggml_tensor* add,
                      float* dst,int ncols,int nrows,int nchannels,int nsamples,float eps,cudaStream_t stream);

bool hbflab_m2_selected_mat(const ggml_tensor*);
bool hbflab_m2_mul_mat(const ggml_tensor*,const ggml_tensor*,ggml_tensor*,cudaStream_t);

bool hbflab_m2_precision(const ggml_tensor*,ggml_tensor*);
