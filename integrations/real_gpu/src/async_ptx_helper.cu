#include "hbflab/async_device.cuh"
static_assert(sizeof(hbflab::AsyncDeviceView)==64,"async PTX view ABI");
extern "C" __device__ __noinline__ unsigned __hbflab_async_gate(hbflab::AsyncDeviceView view,unsigned long long address,
    unsigned bytes,unsigned active,unsigned op,unsigned long long output,unsigned long long size,unsigned long long instance,
    unsigned long long native,unsigned long long native_size) {
    using namespace hbflab;
    if(!instance || view.version!=async_abi_version || !view.mailboxes ||
       blockDim.x!=view.threads || gridDim.x!=view.blocks)return 0;
    auto& slot=view.mailboxes[blockIdx.x*blockDim.x+threadIdx.x];
    if(acquire(slot.terminal))return 0;
    if(op==2 || !active)return 1; // False predicates never touch an address or issue a request.
    if(!op) {
        // Registered HBF bytes always take precedence. Partial overlap cannot
        // turn an invalid registered access into an unmodeled native access.
        bool overlap=false;
        for(unsigned i=0;i<view.range_count;++i){const auto r=view.ranges[i];if(address>=r.gpu_base?address-r.gpu_base<r.bytes:r.gpu_base-address<bytes)overlap=true;}
        if(!overlap && native && bytes && bytes<=16 && address%bytes==0 && address>=native && native_size>=bytes && address-native<=native_size-bytes)return 1;
        return AsyncDeviceClient(view).gate_load(reinterpret_cast<const void*>(address),bytes,bytes)==Status::Ok;
    }
    bool bad=op!=1 || !bytes || bytes>16 || address%bytes || address<output || size<bytes || address-output>size-bytes;
    for(unsigned i=0;i<view.range_count && !bad;++i){const auto r=view.ranges[i];bad=address>=r.gpu_base?address-r.gpu_base<r.bytes:r.gpu_base-address<bytes;}
    if(bad){release(slot.terminal,static_cast<unsigned>(Status::InvalidRange));return 0;}
    return 1;
}
