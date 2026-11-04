// An exact FP32 embedding gather with a small standalone CUDA binary, to avoid
// test whether the original PyTorch embedding triggers excessive NVBit memory use.
#include <cuda_runtime.h>
#include <math_constants.h>
__global__ void embedding_gather(const float* table,const long long* ids,float* output,
                                 long long count,long long rows,int width){
    const long long i=static_cast<long long>(blockIdx.x)*blockDim.x+threadIdx.x;
    if(i>=count*width)return;
    const long long row=ids[i/width];
    output[i]=(row>=0&&row<rows)?table[row*width+i%width]:CUDART_NAN_F;
}
extern "C" int hbf_embedding(const float* table,const long long* ids,float* output,
                              long long count,long long rows,int width,unsigned long long stream){
    embedding_gather<<<(count*width+255)/256,256,0,reinterpret_cast<cudaStream_t>(stream)>>>(table,ids,output,count,rows,width);
    return static_cast<int>(cudaGetLastError());
}
