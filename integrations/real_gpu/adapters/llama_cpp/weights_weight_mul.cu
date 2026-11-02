// Derived from pinned llama.cpp, MIT license: third_party/llama.cpp/LICENSE.
#include <cuda_runtime.h>
#include <cstdint>
__device__ __forceinline__ void ggml_cuda_pdl_lc(){}
__device__ __forceinline__ void ggml_cuda_pdl_sync(){}
__device__ __forceinline__ unsigned fastdiv(unsigned a,uint3 d){return a/d.z;}
__device__ __forceinline__ unsigned fastmodulo(unsigned a,uint3 d){return a%d.z;}
template<class T,class U> __device__ __forceinline__ T ggml_cuda_cast(U x){return static_cast<T>(x);}
static __device__ __forceinline__ float op_mul(float a,float b){return a*b;}
template <float (*bin_op)(const float, const float),
          typename src0_t,
          typename src1_t,
          typename dst_t,
          typename... src1_ptrs>
static __device__ __forceinline__ void k_bin_bcast(const src0_t *         src0,
                                   const src1_t *         src1,
                                   dst_t *                dst,
                                   const uint32_t         ne0,
                                   const uint32_t         ne1,
                                   const uint32_t         ne2,
                                   const uint3            ne3,
                                   const uint3            ne10,
                                   const uint3            ne11,
                                   const uint3            ne12,
                                   const uint3            ne13,
                                 /*const uint32_t         s0,*/
                                   const uint32_t         s1,
                                   const uint32_t         s2,
                                   const uint32_t         s3,
                                   const uint32_t         s00,
                                   const uint32_t         s01,
                                   const uint32_t         s02,
                                   const uint32_t         s03,
                                   const uint32_t         s10,
                                   const uint32_t         s11,
                                   const uint32_t         s12,
                                   const uint32_t         s13,
                                   src1_ptrs... src1s) {
    ggml_cuda_pdl_lc();
    const uint32_t i0s = blockDim.x * blockIdx.x + threadIdx.x;
    const uint32_t i1  = (0u);
    const uint32_t i2  = fastdiv((0u), ne3);
    const uint32_t i3  = (0u) - (i2 * ne3.z);

    if (i0s >= ne0 || i1 >= ne1 || i2 >= ne2 || i3 >= ne3.z) {
        return;
    }

    const uint32_t i11 = fastmodulo(i1, ne11);
    const uint32_t i12 = fastmodulo(i2, ne12);
    const uint32_t i13 = fastmodulo(i3, ne13);

    const size_t i_src0 = size_t( i3)*s03 + size_t( i2)*s02 + size_t( i1)*s01;
    const size_t i_src1 = size_t(i13)*s13 + size_t(i12)*s12 + size_t(i11)*s11;
    const size_t i_dst  = size_t( i3)*s3  + size_t( i2)*s2  + size_t( i1)*s1;

    const src0_t * src0_row = src0 ? (src0 + i_src0) : nullptr;
    dst_t * dst_row = dst + i_dst;

    const uint32_t s0 = blockDim.x * gridDim.x;

    ggml_cuda_pdl_sync();
    for (uint32_t i0 = i0s; i0 < ne0; i0 += s0) {
        const uint32_t i10 = fastmodulo(i0, ne10);

        float result = src0_row ? ggml_cuda_cast<float>(src0_row[size_t(i0)*s00]) : 0.0f;
        if constexpr (sizeof...(src1_ptrs) > 0) {
            result = (..., (result = bin_op(result, ggml_cuda_cast<float>(src1s[i_src1 + size_t(i10)*s10]))));
        } else {
            result = bin_op(result, ggml_cuda_cast<float>(src1[i_src1 + size_t(i10)*s10]));
        }

        dst_row[i0] = ggml_cuda_cast<dst_t>(result);

        // protect i0 from overflow
        if (ne0 - i0 <= s0) {
           break;
        }
    }
}


extern "C" __global__ void m5_weight_mul(const float* activation,const float* weight,float* output) {
    k_bin_bcast<op_mul,float,float,float>(activation,weight,output,
        1024,1,1,make_uint3(0,0,1),make_uint3(0,0,1024),make_uint3(0,0,1),make_uint3(0,0,1),make_uint3(0,0,1),
        1024,1024,1024,1,1024,1024,1024,1,1024,1024,1024);
}
