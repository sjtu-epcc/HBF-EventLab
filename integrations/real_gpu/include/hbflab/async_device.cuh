#pragma once
#include "hbflab/async_protocol.hpp"
#include "hbflab/device.cuh"
#include <type_traits>
namespace hbflab {
// Independent per-thread producer, including divergent/early-return threads.
// Scalar calls have no collective; warp_load_contiguous is explicitly collective.
// The launch owns a disjoint immutable slot range.
class AsyncDeviceClient {
    AsyncDeviceView view_;
    __device__ Status access(const void* source,void* output,unsigned bytes,unsigned alignment,AsyncAccessTrace* trace,bool gate_only=false) {
        AsyncAccessTrace t{};t.wanted_ns=gpu_now_ns();
        if(view_.version!=async_abi_version || !view_.mailboxes || !view_.ranges || !view_.session ||
           !view_.range_count || view_.range_count>max_ranges || !view_.timeout_ns ||
           gridDim.x!=view_.blocks || gridDim.y!=1 || gridDim.z!=1 ||
           blockDim.x!=view_.threads || blockDim.y!=1 || blockDim.z!=1) return Status::Unsupported;
        const auto local=blockIdx.x*blockDim.x+threadIdx.x;
        auto& slot=view_.mailboxes[local];
        auto finish=[&](Status status) {
            t.access.status=status;t.access.resumed_ns=gpu_now_ns();
            if(status!=Status::Ok)release(slot.terminal,static_cast<std::uint32_t>(status));
            if(trace)*trace=t;
            return status;
        };
        auto health=[&]() {
            if(const auto terminal=acquire(slot.terminal))return static_cast<Status>(terminal);
            return gpu_now_ns()-t.wanted_ns>=view_.timeout_ns?Status::Timeout:Status::Ok;
        };
        Span span{};bool valid=false;
        const auto address=reinterpret_cast<std::uint64_t>(source);
        t.address=address;t.bytes=bytes;
        if(source && (output || gate_only) && bytes && bytes<=max_read_bytes && alignment && address%alignment==0) {
            for(unsigned i=0;i<view_.range_count;++i) {
                const auto r=view_.ranges[i];
                if(address>=r.gpu_base && bytes<=r.bytes && address-r.gpu_base<=r.bytes-bytes) {
                    span={address,bytes,i};valid=true;break;
                }
            }
        }
        if(!valid)return finish(Status::InvalidRange);
        const auto last=acquire(slot.want_sequence);
        if(last==~std::uint64_t{0} || acquire(slot.done_sequence)!=last)return finish(Status::ProtocolError);
        const auto ticket=last+1;
        t.session=view_.session;t.launch=view_.launch;t.ticket=ticket;t.producer=view_.producer_base+local;
        release(slot.want_sequence,ticket);
        while(acquire(slot.grant_sequence)!=ticket) {
            const auto h=health();if(h!=Status::Ok)return finish(h);__nanosleep(128);
        }
        t.granted_ns=gpu_now_ns();t.floor_ns=slot.floor_ns;
        while(gpu_now_ns()<t.floor_ns) {
            const auto h=health();if(h!=Status::Ok)return finish(h);__nanosleep(128);
        }
        const auto h=health();if(h!=Status::Ok)return finish(h);
        AsyncRequest r{};r.session=view_.session;r.launch=view_.launch;
        r.read.ticket=ticket;r.read.producer=view_.producer_base+local;
        r.read.arrival_ns=gpu_now_ns();r.read.span_count=1;r.read.spans[0]=span;
        t.access.arrival_ns=r.read.arrival_ns;
        slot.request=r;release(slot.request_sequence,ticket);
        while(acquire(slot.done_sequence)!=ticket) {
            const auto s=health();if(s!=Status::Ok)return finish(s);__nanosleep(128);
        }
        t.access.completion_seen_ns=gpu_now_ns();const auto c=slot.completion;
        if(c.version!=abi_version || c.ticket!=ticket)return finish(Status::ProtocolError);
        if(c.status!=Status::Ok)return finish(c.status);
        t.access.modeled_ps=c.modeled_ps;t.access.delay_ns=c.delay_ns;
        while(gpu_now_ns()-t.access.arrival_ns<c.delay_ns) {
            const auto s=health();if(s!=Status::Ok)return finish(s);__nanosleep(128);
        }
        const auto end=health();if(end!=Status::Ok)return finish(end);
        const auto* from=static_cast<const volatile unsigned char*>(source);
        auto* to=static_cast<unsigned char*>(output);
        if(output)for(unsigned i=0;i<bytes;++i)to[i]=from[i];
        return finish(Status::Ok);
    }
public:
    __device__ explicit AsyncDeviceClient(AsyncDeviceView v):view_(v){}
    __device__ Status read_span(const void* source,void* output,unsigned bytes,AsyncAccessTrace* trace=nullptr) {
        return access(source,output,bytes,1,trace);
    }
    template<class T> __device__ Status load(const T* source,T& output,AsyncAccessTrace* trace=nullptr) {
        static_assert(std::is_trivially_copyable<T>::value && (sizeof(T)==1 || sizeof(T)==2 || sizeof(T)==4 || sizeof(T)==8 || sizeof(T)==16),"unsupported asynchronous load type");
        return access(source,&output,sizeof(T),alignof(T),trace);
    }
    __device__ Status gate_load(const void* source,unsigned bytes,unsigned alignment) {
        return access(source,nullptr,bytes,alignment,nullptr,true);
    }
    // All lanes named in mask must call with the same mask. In increasing lane
    // order their addresses must form one exact, contiguous typed span. Only
    // the lowest lane owns a request; followers stay Parked, never timestamp.
    // trace is written ONLY by the leader and describes the combined request.
    template<class T> __device__ Status warp_load_contiguous(const T* source,T& output,unsigned mask,
                                                            AsyncAccessTrace* trace=nullptr) {
        static_assert(std::is_trivially_copyable<T>::value && (sizeof(T)==1 || sizeof(T)==2 || sizeof(T)==4 || sizeof(T)==8 || sizeof(T)==16),"unsupported warp load type");
        const unsigned lane=threadIdx.x&31;
        if(!mask || !(mask&(1u<<lane)))return Status::Unsupported;
        const unsigned leader=__ffs(mask)-1,rank=__popc(mask&((1u<<lane)-1));
        const auto address=reinterpret_cast<unsigned long long>(source);
        const auto base=__shfl_sync(mask,address,leader);
        const unsigned bytes=__popc(mask)*sizeof(T);
        const bool valid=base && base<=~0ull-bytes && address==base+rank*sizeof(T) && address%alignof(T)==0;
        const bool all_valid=__all_sync(mask,valid);
        Status result=Status::Unsupported;
        if(lane==leader) {
            // Invalid layouts still enter the ordinary checked failure path,
            // so the host terminates instead of accepting a missing access.
            result=access(all_valid?reinterpret_cast<const void*>(base):nullptr,nullptr,bytes,alignof(T),trace,true);
        }
        result=static_cast<Status>(__shfl_sync(mask,static_cast<unsigned>(result),leader));
        if(result!=Status::Ok)return result;
        __syncwarp(mask);
        const auto* from=reinterpret_cast<const volatile unsigned char*>(source);
        auto* to=reinterpret_cast<unsigned char*>(&output);
        for(unsigned i=0;i<sizeof(T);++i)to[i]=from[i];
        return Status::Ok;
    }
};
}
