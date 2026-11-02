#include "hbflab/async_runtime.hpp"
#include <string>
#include "hbflab/async_order.hpp"
#include "hbflab/device.cuh"
#include <cuda.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <new>
#include <thread>

namespace hbflab {
namespace {
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
__global__ void read_epoch(std::uint64_t* output){*output=gpu_now_ns();}
std::atomic<std::uint64_t> next_session{1};
std::uint64_t wall_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
}
struct AsyncRuntime::Impl {
    struct Launch {
        AsyncDeviceView view{};cudaStream_t stream=nullptr;cudaEvent_t event=nullptr;
        bool launched=false,finished=false;
        std::uint64_t started_at=0;
    };
    AsyncRuntimeOptions options;TimingBackend backend;
    std::uint64_t session=next_session.fetch_add(1);
    CUcontext context=nullptr;int ordinal=0;
    const std::thread::id owner=std::this_thread::get_id();
    AsyncMailbox* host=nullptr;AsyncMailbox* device=nullptr;Range* device_ranges=nullptr;
    std::vector<Range> ranges;std::vector<void*> allocations;std::vector<Launch> launches;
    std::vector<std::uint32_t> owners;
    std::vector<std::uint64_t> admitted_at;
    std::vector<std::uint64_t> dispatches;
    std::vector<std::size_t> launch_inflight;
    std::vector<TimedRead> pending;
    std::unique_ptr<AsyncOrder> order;
    std::deque<AsyncServiceTrace> traces;
    std::deque<AsyncMediaTrace> media_traces;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> media_ranges;
    std::uint64_t wall_epoch=0,dispatch_serial=0;
    struct Media {
        bool active=false,done=false,write=false;
        std::uint64_t logical=0,ticket=0,dispatch=0,ready_wall_ns=0;
        std::uint32_t bytes=0;
        std::array<std::uint64_t,64> tokens{};
        ReadResult read_result{};WriteResult write_result{};
    } media;
    AsyncRuntimeStats stats;
    std::size_t inflight=0;
    bool prepared=false,served=false,poisoned=false,live=false;
    std::uint64_t next_launch=1;
    explicit Impl(AsyncRuntimeOptions o):options(o),backend(o.backend) {
        if(!session || !o.time_scale || o.time_scale>1000000 || !o.timeout_ns || o.timeout_ns>1000000000 ||
           !o.service_timeout_ns || o.service_timeout_ns>60000000000ull || o.audit_limit>1000000)
            throw std::invalid_argument("invalid asynchronous runtime limits");
        ck(cudaFree(nullptr));ck(cudaGetDevice(&ordinal));
        if(cuCtxGetCurrent(&context)!=CUDA_SUCCESS || !context)throw std::runtime_error("missing CUDA context");
        cudaDeviceProp p{};ck(cudaGetDeviceProperties(&p,ordinal));
        if(!p.canMapHostMemory || !p.unifiedAddressing || p.major<7)throw std::runtime_error("asynchronous bridge requires mapped memory/UVA/SM70+");
    }
    void current() const {
        CUcontext ctx=nullptr;int dev=-1;ck(cudaGetDevice(&dev));
        if(owner!=std::this_thread::get_id() || dev!=ordinal || cuCtxGetCurrent(&ctx)!=CUDA_SUCCESS || ctx!=context)
            throw std::runtime_error("asynchronous runtime owner/context mismatch");
    }
    void fail(Status status) {
        poisoned=true;
        if(host)for(std::size_t i=0;i<owners.size();++i)release(host[i].terminal,static_cast<std::uint32_t>(status));
    }
    ~Impl() {
        if(!context)return;
        CUcontext previous=nullptr;cuCtxGetCurrent(&previous);
        if(cuCtxSetCurrent(context)!=CUDA_SUCCESS)return;
        if(host)fail(Status::ProtocolError);
        for(auto& l:launches){if(l.launched)cudaEventSynchronize(l.event);if(l.event)cudaEventDestroy(l.event);}
        for(auto p:allocations)cudaFree(p);
        if(device_ranges)cudaFree(device_ranges);
        if(host)cudaFreeHost(host);
        cuCtxSetCurrent(previous);
    }
    void complete_media(const ReadResult& r,std::uint32_t replayed=0) {
        const auto producer=static_cast<unsigned>(owners.size());const auto state=order->slot(producer);
        if(!media.active || media.done || state.ticket!=media.ticket || r.status!=Status::Ok)
            throw std::runtime_error("shared media completion mismatch");
        media.read_result=r;media.write_result={r.status,r.begin_ps,r.end_ps,replayed};
        if(options.audit_limit) {
            if(media_traces.size()==options.audit_limit){media_traces.pop_front();++stats.audit_dropped;}
            media_traces.push_back({media.ticket,media.dispatch,state.arrival_ns,state.arrival_ps,r.end_ps,state.floor_ns,
                                   media.logical,producer,media.bytes,replayed,media.write,
                                   media.ready_wall_ns,wall_ns(),0,scale_ticks(r.end_ps-r.begin_ps,options.time_scale,1000,true)});
        }else ++stats.audit_dropped;
        order->complete(producer,media.ticket);media.done=true;++stats.media_completed;
    }
    void begin_media(std::uint64_t logical,std::uint32_t bytes,const std::array<std::uint64_t,64>* tokens) {
        current();
        if(!live || served || poisoned || media.active || media_ranges.empty())throw std::runtime_error("shared media session unavailable");
        bool valid=false;
        for(const auto& range:media_ranges)
            if(logical>=range.first && bytes<=range.second && logical-range.first<=range.second-bytes)valid=true;
        if(!bytes || bytes>4096 || !valid || (tokens && (bytes!=4096 || logical%4096)))
            throw std::invalid_argument("shared media range invalid");
        const auto producer=static_cast<unsigned>(owners.size());
        const auto ticket=hbf_eventlab::checked_add(order->slot(producer).ticket,1);
        order->grant(producer,ticket,backend.now_ps());
        media={};media.active=true;media.logical=logical;media.bytes=bytes;media.ticket=ticket;media.write=tokens!=nullptr;
        if(tokens)media.tokens=*tokens;
    }
    Status poll() {
        for(std::uint32_t i=0;i<owners.size();++i) {
            if(owners[i]==UINT32_MAX)continue;
            auto& m=host[i];const auto& l=launches[owners[i]];
            if(const auto terminal=acquire(m.terminal))
                return terminal<=static_cast<unsigned>(Status::ProtocolError)?static_cast<Status>(terminal):Status::ProtocolError;
            auto state=order->slot(i);
            const auto want=acquire(m.want_sequence);
            if(state.state==AsyncOrder::State::Parked && want!=state.ticket) {
                if(l.finished)return Status::ProtocolError;
                const auto floor=order->grant(i,want,backend.now_ps());
                admitted_at[i]=wall_ns();m.floor_ns=floor;release(m.grant_sequence,want);++stats.grants;
                state=order->slot(i);
            }else if(want!=state.ticket)return Status::ProtocolError;
            const auto request=acquire(m.request_sequence);
            if(state.state==AsyncOrder::State::Granted) {
                if(wall_ns()-admitted_at[i]>=options.timeout_ns)return Status::Timeout;
                if(request==state.ticket) {
                    const auto r=m.request;
                    if(r.session!=session || r.launch!=l.view.launch || r.read.version!=abi_version ||
                       r.read.producer!=i || r.read.ticket!=state.ticket || r.read.status!=Status::Ok ||
                       !r.read.span_count || r.read.span_count>max_spans)return Status::ProtocolError;
                    TimedRead work{0,{}};std::uint64_t bytes=0;
                    for(unsigned j=0;j<r.read.span_count;++j) {
                        const auto& span=r.read.spans[j];
                        if(span.range>=ranges.size())return Status::InvalidRange;
                        const auto& range=ranges[span.range];
                        if(!span.bytes || span.bytes>max_read_bytes || span.address<range.gpu_base ||
                           span.bytes>range.bytes || span.address-range.gpu_base>range.bytes-span.bytes)return Status::InvalidRange;
                        bytes+=span.bytes;work.spans.push_back({range.logical_base+span.address-range.gpu_base,span.bytes});
                    }
                    if(bytes>max_read_bytes)return Status::InvalidRange;
                    order->publish(i,state.ticket,r.read.arrival_ns);
                    work.arrival_ps=order->slot(i).arrival_ps;pending[i]=std::move(work);
                }else if(request!=state.ticket-1)return Status::ProtocolError;
            }else if(request!=state.ticket)return Status::ProtocolError;
        }
        if(media.active && !media.done) {
            const auto producer=static_cast<unsigned>(owners.size());const auto state=order->slot(producer);
            if(state.state==AsyncOrder::State::Granted) {
                // Correlate CPU readiness to the sampled GPU epoch, then wait
                // for admission exactly as a GPU source does. Never backdate
                // or silently clamp a published request to a future floor.
                const auto wall=wall_ns();const auto correlated=hbf_eventlab::checked_add(stats.gpu_epoch_ns,wall-wall_epoch);
                if(correlated>=state.floor_ns){order->publish(producer,state.ticket,correlated);media.ready_wall_ns=wall;}
            }
        }
        for(const auto& ready:order->ready()) {
            if(ready.producer==owners.size()) {
                const auto id=std::uint64_t{ready.producer}+1;
                if(media.write)backend.submit_write(id,ready.arrival_ps,media.logical,media.tokens);
                else backend.submit_read(id,{ready.arrival_ps,{{media.logical,media.bytes}}});
                media.dispatch=++dispatch_serial;++stats.media_submitted;continue;
            }
            backend.submit_read(std::uint64_t{ready.producer}+1,pending[ready.producer]);
            ++stats.submitted;++inflight;stats.peak_inflight=std::max(stats.peak_inflight,inflight);
            dispatches[ready.producer]=++dispatch_serial;++launch_inflight[owners[ready.producer]];
        }
        std::size_t active_launches=0;
        for(auto count:launch_inflight)if(count)++active_launches;
        stats.peak_active_launches=std::max(stats.peak_active_launches,active_launches);
        if(media.active && !media.done)stats.peak_gpu_during_media=std::max(stats.peak_gpu_during_media,inflight);
        backend.advance_before(order->unknown_floor());
        for(const auto& completed:backend.take_completed()) {
            const auto i=static_cast<std::uint32_t>(completed.id-1);
            if(i==owners.size()){complete_media(completed.result);continue;}
            const auto s=order->slot(i);
            const auto& r=completed.result;if(r.status!=Status::Ok)return r.status;
            Completion c{};c.ticket=s.ticket;c.status=Status::Ok;c.modeled_ps=r.end_ps-r.begin_ps;
            c.delay_ns=scale_ticks(c.modeled_ps,options.time_scale,1000,true);
            if(c.delay_ns>=options.timeout_ns)return Status::Timeout;
            if(options.audit_limit) {
                if(traces.size()==options.audit_limit){traces.pop_front();++stats.audit_dropped;}
                const auto& request=host[i].request.read;
                traces.push_back({launches[owners[i]].view.launch,s.ticket,s.arrival_ns,s.arrival_ps,r.end_ps,i,r.sectors,dispatches[i],s.floor_ns,
                                  request.spans[0].address,request.spans[0].bytes,request.span_count});
            }else ++stats.audit_dropped;
            order->complete(i,s.ticket);host[i].completion=c;release(host[i].done_sequence,s.ticket);
            ++stats.completed;--inflight;--launch_inflight[owners[i]];
        }
        for(const auto& completed:backend.take_completed_writes()) {
            if(completed.id!=owners.size()+1 || !media.write)throw std::runtime_error("unexpected shared write completion");
            const auto& w=completed.result;complete_media({w.status,w.begin_ps,w.end_ps,0},w.replayed_pages);
        }
        return Status::Ok;
    }
};
AsyncRuntime::AsyncRuntime(AsyncRuntimeOptions o):impl_(std::make_unique<Impl>(o)){}
AsyncRuntime::~AsyncRuntime()=default;
void AsyncRuntime::register_readonly(const void* source,std::size_t bytes) {
    auto& s=*impl_;s.current();
    if(s.prepared || s.poisoned || !source || !bytes || s.ranges.size()==max_ranges)
        throw std::invalid_argument("invalid asynchronous borrowed range");
    const auto address=reinterpret_cast<CUdeviceptr>(source);
    CUdeviceptr base=0;std::size_t size=0;CUcontext context=nullptr;
    CUmemorytype kind{};
    if(cuPointerGetAttribute(&context,CU_POINTER_ATTRIBUTE_CONTEXT,address)!=CUDA_SUCCESS || context!=s.context ||
       cuPointerGetAttribute(&kind,CU_POINTER_ATTRIBUTE_MEMORY_TYPE,address)!=CUDA_SUCCESS || kind!=CU_MEMORYTYPE_DEVICE ||
       cuMemGetAddressRange(&base,&size,address)!=CUDA_SUCCESS || address<base || bytes>size || address-base>size-bytes)
        throw std::invalid_argument("borrow requires device allocation bounds in this context");
    for(const auto& r:s.ranges)if(address>=r.gpu_base?address-r.gpu_base<r.bytes:r.gpu_base-address<bytes)
        throw std::invalid_argument("borrow overlaps registered range");
    const auto padded=hbf_eventlab::checked_mul(hbf_eventlab::checked_add(bytes,4095)/4096,4096);
    try{s.ranges.push_back({address,bytes,s.backend.reserve(padded)});}
    catch(...){s.fail(Status::BackendError);throw;}
}
std::uint64_t AsyncRuntime::reserve_media(std::uint64_t bytes) {
    auto& s=*impl_;s.current();
    if(s.prepared || s.poisoned || s.media_ranges.size()>=16)throw std::runtime_error("media registration is frozen or full");
    const auto base=s.backend.reserve(bytes);s.media_ranges.push_back({base,bytes});return base;
}
ReadResult AsyncRuntime::read_media(std::uint64_t logical,std::uint32_t bytes) {
    auto& s=*impl_;s.begin_media(logical,bytes,nullptr);const auto start=wall_ns();
    try {
        for(;;) {
            if(poll_live()!=Status::Ok)throw std::runtime_error("shared read terminal failure");
            if(wall_ns()-start>=s.options.service_timeout_ns)throw std::runtime_error("shared read timeout");
            if(s.media.done && wall_ns()-s.media.ready_wall_ns>=scale_ticks(s.media.read_result.end_ps-s.media.read_result.begin_ps,s.options.time_scale,1000,true))break;
            std::this_thread::yield();
        }
        if(!s.media_traces.empty() && s.media_traces.back().ticket==s.media.ticket)s.media_traces.back().resumed_wall_ns=wall_ns();
        s.media.active=false;return s.media.read_result;
    }catch(...){s.fail(Status::BackendError);throw;}
}
WriteResult AsyncRuntime::write_media(std::uint64_t logical,const std::array<std::uint64_t,64>& tokens) {
    auto& s=*impl_;s.begin_media(logical,4096,&tokens);const auto start=wall_ns();
    try {
        for(;;) {
            if(poll_live()!=Status::Ok)throw std::runtime_error("shared write terminal failure");
            if(wall_ns()-start>=s.options.service_timeout_ns)throw std::runtime_error("shared write timeout");
            if(s.media.done && wall_ns()-s.media.ready_wall_ns>=scale_ticks(s.media.write_result.end_ps-s.media.write_result.begin_ps,s.options.time_scale,1000,true))break;
            std::this_thread::yield();
        }
        if(!s.media_traces.empty() && s.media_traces.back().ticket==s.media.ticket)s.media_traces.back().resumed_wall_ns=wall_ns();
        s.media.active=false;return s.media.write_result;
    }catch(...){s.fail(Status::BackendError);throw;}
}
std::vector<AsyncMediaTrace> AsyncRuntime::take_media_audit() {
    auto& s=*impl_;s.current();std::vector<AsyncMediaTrace> result(s.media_traces.begin(),s.media_traces.end());s.media_traces.clear();return result;
}
void AsyncRuntime::abort(){auto& s=*impl_;s.current();s.fail(Status::BackendError);}
const void* AsyncRuntime::upload(const void* source,std::size_t bytes) {
    auto& s=*impl_;s.current();
    if(s.prepared || s.poisoned || !source || !bytes || s.ranges.size()==max_ranges)throw std::invalid_argument("invalid asynchronous upload");
    const auto padded=hbf_eventlab::checked_mul(hbf_eventlab::checked_add(bytes,4095)/4096,4096);
    void* p=nullptr;ck(cudaMalloc(&p,bytes));
    try {
        ck(cudaMemcpy(p,source,bytes,cudaMemcpyHostToDevice));const auto logical=s.backend.reserve(padded);
        s.allocations.push_back(p);
        try{s.ranges.push_back({reinterpret_cast<std::uint64_t>(p),bytes,logical});}
        catch(...){s.allocations.pop_back();throw;}
    }catch(...){cudaFree(p);s.fail(Status::BackendError);throw;}
    return p;
}
void AsyncRuntime::prepare(const std::vector<AsyncLaunchShape>& shapes) {
    auto& s=*impl_;s.current();
    if(s.prepared || s.poisoned || s.ranges.empty() || shapes.empty() || shapes.size()>256)throw std::invalid_argument("invalid asynchronous preparation");
    cudaDeviceProp prop{};ck(cudaGetDeviceProperties(&prop,s.ordinal));
    std::uint64_t total=0;
    for(const auto shape:shapes) {
        if(!shape.blocks || !shape.threads || shape.threads>static_cast<unsigned>(prop.maxThreadsPerBlock))throw std::invalid_argument("invalid asynchronous launch shape");
        total+=std::uint64_t{shape.blocks}*shape.threads;
        if(total+(s.media_ranges.empty()?0:1)>max_async_producers)throw std::invalid_argument("asynchronous producer bound exceeded");
    }
    std::uint64_t* epoch=nullptr;
    try {
        s.owners.resize(total);s.admitted_at.resize(total);s.pending.resize(total);s.dispatches.resize(total);
        s.launch_inflight.resize(shapes.size());
        ck(cudaHostAlloc(reinterpret_cast<void**>(&s.host),total*sizeof(AsyncMailbox),cudaHostAllocMapped));
        for(std::size_t i=0;i<total;++i)new(s.host+i)AsyncMailbox{};
        ck(cudaHostGetDevicePointer(reinterpret_cast<void**>(&s.device),s.host,0));
        ck(cudaMalloc(reinterpret_cast<void**>(&s.device_ranges),s.ranges.size()*sizeof(Range)));
        ck(cudaMemcpy(s.device_ranges,s.ranges.data(),s.ranges.size()*sizeof(Range),cudaMemcpyHostToDevice));
        ck(cudaMalloc(reinterpret_cast<void**>(&epoch),sizeof(std::uint64_t)));
        read_epoch<<<1,1>>>(epoch);ck(cudaGetLastError());std::uint64_t gpu=0;
        ck(cudaMemcpy(&gpu,epoch,sizeof(gpu),cudaMemcpyDeviceToHost));ck(cudaFree(epoch));epoch=nullptr;
        s.wall_epoch=wall_ns();
        const auto sources=static_cast<unsigned>(total)+(s.media_ranges.empty()?0u:1u);
        s.order=std::make_unique<AsyncOrder>(sources,gpu,s.backend.now_ps(),s.options.time_scale);
        s.stats.gpu_epoch_ns=gpu;s.stats.model_epoch_ps=s.backend.now_ps();
        s.launches.resize(shapes.size());unsigned base=0;
        for(unsigned i=0;i<shapes.size();++i) {
            auto& l=s.launches[i];const auto shape=shapes[i];
            l.view={s.device+base,s.device_ranges,s.session,std::uint64_t{i}+1,s.options.timeout_ns,
                    static_cast<unsigned>(s.ranges.size()),shape.blocks,shape.threads,base,async_abi_version};
            ck(cudaEventCreateWithFlags(&l.event,cudaEventDisableTiming));
            for(unsigned j=0;j<shape.blocks*shape.threads;++j)s.owners[base+j]=i;
            base+=shape.blocks*shape.threads;
        }
        s.backend.begin_stream(total+(s.media_ranges.empty()?0:1));s.prepared=true;
    }catch(...){if(epoch)cudaFree(epoch);s.fail(Status::BackendError);throw;}
}
void AsyncRuntime::launch(std::uint32_t index,const void* kernel,const std::vector<void*>& args,cudaStream_t stream) {
    auto& s=*impl_;s.current();
    if(!s.prepared || s.live || s.served || s.poisoned || index>=s.launches.size() || !kernel || args.size()>64)
        throw std::invalid_argument("invalid asynchronous launch");
    auto& l=s.launches[index];if(l.launched)throw std::invalid_argument("launch identity already used");
    cudaStreamCaptureStatus capture{};ck(cudaStreamIsCapturing(stream,&capture));
    if(capture!=cudaStreamCaptureStatusNone)throw std::invalid_argument("asynchronous service cannot participate in graph capture");
    std::vector<void*> actual{&l.view};actual.insert(actual.end(),args.begin(),args.end());
    try {
        ck(cudaLaunchKernel(kernel,dim3(l.view.blocks),dim3(l.view.threads),actual.data(),0,stream));
        l.launched=true;l.stream=stream;ck(cudaEventRecord(l.event,stream));
    }catch(...){s.fail(Status::BackendError);if(l.launched)cudaStreamSynchronize(stream);throw;}
}
Status AsyncRuntime::serve() {
    auto& s=*impl_;s.current();
    if(!s.prepared || s.live || s.served || s.poisoned)throw std::runtime_error("invalid asynchronous service state");
    for(const auto& l:s.launches)if(!l.launched)throw std::runtime_error("launch all prepared kernels before service");
    s.served=true;Status status=Status::Ok;const auto start=wall_ns();
    try {
        for(;;) {
            if(wall_ns()-start>=s.options.service_timeout_ns){status=Status::Timeout;break;}
            status=s.poll();if(status!=Status::Ok)break;
            bool all=true;
            for(auto& l:s.launches) {
                if(!l.finished) {
                    const auto q=cudaEventQuery(l.event);
                    if(q==cudaSuccess) {
                        l.finished=true;
                        for(unsigned n=0;n<l.view.blocks*l.view.threads;++n) {
                            const auto i=l.view.producer_base+n;const auto state=s.order->slot(i);
                            if(const auto t=acquire(s.host[i].terminal)) {
                                status=t<=static_cast<unsigned>(Status::ProtocolError)?static_cast<Status>(t):Status::ProtocolError;break;
                            }
                            if(state.state!=AsyncOrder::State::Parked || acquire(s.host[i].want_sequence)!=state.ticket ||
                               acquire(s.host[i].request_sequence)!=state.ticket || acquire(s.host[i].done_sequence)!=state.ticket) {
                                status=Status::ProtocolError;break;
                            }
                        }
                    }
                    else if(q!=cudaErrorNotReady)ck(q);
                }
                all=all && l.finished;
            }
            if(status!=Status::Ok)break;
            if(all) {
                // Completion events acquire final device publications. Recheck
                // for a producer that exited with an unpublished or unserved request.
                status=s.poll();if(status!=Status::Ok)break;
                if(!s.order->idle()){status=Status::ProtocolError;break;}
                while(s.backend.advance_before(UINT64_MAX)){}
                s.backend.end_stream();s.backend.finish();break;
            }
            std::this_thread::yield();
        }
    }catch(const std::invalid_argument&){status=Status::ProtocolError;}
     catch(...){status=Status::BackendError;}
    if(status!=Status::Ok)s.fail(status);
    for(auto& l:s.launches)if(cudaEventSynchronize(l.event)!=cudaSuccess){status=Status::BackendError;s.fail(status);}
    return status;
}
void AsyncRuntime::start_live(std::uint32_t producers,std::uint32_t limit) {
    if(!limit || limit>256)throw std::invalid_argument("invalid live launch bound");
    prepare({{producers,1}});
    auto& s=*impl_;
    try {
        ck(cudaEventDestroy(s.launches[0].event));s.launches[0].event=nullptr;
        s.launches.clear();s.launches.resize(limit);s.launch_inflight.assign(limit,0);
        std::fill(s.owners.begin(),s.owners.end(),UINT32_MAX);
        for(auto& l:s.launches)ck(cudaEventCreateWithFlags(&l.event,cudaEventDisableTiming));
        s.live=true;
    }catch(...){s.fail(Status::BackendError);throw;}
}
std::uint64_t AsyncRuntime::launch_live(AsyncLaunchShape shape,const void* kernel,const std::vector<void*>& args,cudaStream_t stream,std::size_t shared_bytes) {
    return launch_live_impl(shape,kernel,args,stream,shared_bytes,false);
}
std::uint64_t AsyncRuntime::launch_driver_live(AsyncLaunchShape shape,std::uintptr_t function,const std::vector<void*>& args,cudaStream_t stream) {
    return launch_live_impl(shape,reinterpret_cast<const void*>(function),args,stream,0,true);
}
std::uint64_t AsyncRuntime::launch_live_impl(AsyncLaunchShape shape,const void* kernel,const std::vector<void*>& args,
                                      cudaStream_t stream,std::size_t shared_bytes,bool driver) {
    auto& s=*impl_;s.current();
    if(!s.live || s.served || s.poisoned)throw std::runtime_error("live session unavailable");
    cudaDeviceProp prop{};ck(cudaGetDeviceProperties(&prop,s.ordinal));
    const auto count=std::uint64_t{shape.blocks}*shape.threads;
    if(!kernel || args.size()>64 || !shape.blocks || !shape.threads ||
       shape.threads>static_cast<unsigned>(prop.maxThreadsPerBlock) || count>s.owners.size() || shared_bytes>prop.sharedMemPerBlock)
        throw std::invalid_argument("invalid live launch shape/arguments");
    cudaStreamCaptureStatus capture{};ck(cudaStreamIsCapturing(stream,&capture));
    if(capture!=cudaStreamCaptureStatusNone)throw std::invalid_argument("live graph capture unsupported");
    std::size_t li=0;while(li<s.launches.size() && s.launches[li].launched)++li;
    if(li==s.launches.size())return 0;
    std::size_t run=0,base=0;
    for(std::size_t i=0;i<s.owners.size();++i) {
        run=s.owners[i]==UINT32_MAX?run+1:0;
        if(run==count){base=i+1-count;break;}
    }
    if(run!=count)return 0;
    if(s.next_launch==UINT64_MAX)throw std::overflow_error("live launch identity exhausted");
    auto& l=s.launches[li];l.finished=false;l.stream=stream;l.started_at=wall_ns();
    l.view={s.device+base,s.device_ranges,s.session,s.next_launch++,s.options.timeout_ns,
            static_cast<unsigned>(s.ranges.size()),shape.blocks,shape.threads,static_cast<unsigned>(base),async_abi_version};
    for(std::size_t i=base;i<base+count;++i)s.owners[i]=static_cast<unsigned>(li);
    std::vector<void*> actual{&l.view};actual.insert(actual.end(),args.begin(),args.end());
    try {
        if(driver) {
            const auto code=cuLaunchKernel(reinterpret_cast<CUfunction>(const_cast<void*>(kernel)),shape.blocks,1,1,shape.threads,1,1,
                static_cast<unsigned>(shared_bytes),reinterpret_cast<CUstream>(stream),actual.data(),nullptr);
            if(code!=CUDA_SUCCESS)throw std::runtime_error("asynchronous Driver launch failed: "+std::to_string(static_cast<int>(code)));
        }else ck(cudaLaunchKernel(kernel,dim3(shape.blocks),dim3(shape.threads),actual.data(),shared_bytes,stream));
        l.launched=true;ck(cudaEventRecord(l.event,stream));++s.stats.launches_submitted;
    }catch(...){s.fail(Status::BackendError);if(l.launched)cudaStreamSynchronize(stream);throw;}
    return l.view.launch;
}
Status AsyncRuntime::poll_live() {
    auto& s=*impl_;s.current();
    if(!s.live || s.served || s.poisoned)throw std::runtime_error("live polling unavailable");
    Status status=Status::Ok;
    try {
        status=s.poll();
        if(status==Status::Ok)for(auto& l:s.launches)if(l.launched) {
            const auto q=cudaEventQuery(l.event);
            if(q==cudaErrorNotReady) {
                if(wall_ns()-l.started_at>=s.options.service_timeout_ns){status=Status::Timeout;break;}
                continue;
            }
            ck(q);
            for(unsigned j=0;j<l.view.blocks*l.view.threads;++j) {
                const auto i=l.view.producer_base+j;const auto& state=s.order->slot(i);const auto& m=s.host[i];
                if(const auto t=acquire(m.terminal)){status=t<=5?static_cast<Status>(t):Status::ProtocolError;break;}
                if(state.state!=AsyncOrder::State::Parked || acquire(m.want_sequence)!=state.ticket ||
                   acquire(m.request_sequence)!=state.ticket || acquire(m.done_sequence)!=state.ticket) {
                    status=Status::ProtocolError;break;
                }
            }
            if(status!=Status::Ok)break;
            // The completion event proves the last response was consumed and no
            // old device instruction can access these slots after reclamation.
            for(unsigned j=0;j<l.view.blocks*l.view.threads;++j)s.owners[l.view.producer_base+j]=UINT32_MAX;
            l.finished=true;l.launched=false;++s.stats.launches_completed;
        }
    }catch(const std::invalid_argument&){status=Status::ProtocolError;}
     catch(...){status=Status::BackendError;}
    if(status!=Status::Ok)s.fail(status);
    return status;
}
bool AsyncRuntime::live_idle() const {
    const auto& s=*impl_;s.current();
    if(!s.live || s.poisoned)return false;
    for(const auto& l:s.launches)if(l.launched)return false;
    return s.order->idle();
}
void AsyncRuntime::finish_live() {
    auto& s=*impl_;s.current();
    if(s.served || !live_idle())throw std::runtime_error("finish requires idle live session");
    try {while(s.backend.advance_before(UINT64_MAX)){}s.backend.end_stream();s.backend.finish();s.served=true;}
    catch(...){s.fail(Status::BackendError);throw;}
}
AsyncRuntimeStats AsyncRuntime::statistics() const {auto r=impl_->stats;r.backend=impl_->backend.statistics();r.model_now_ps=impl_->backend.now_ps();return r;}
std::vector<AsyncServiceTrace> AsyncRuntime::audit() const {return {impl_->traces.begin(),impl_->traces.end()};}
std::vector<AsyncServiceTrace> AsyncRuntime::take_audit() {
    auto& s=*impl_;s.current();
    std::vector<AsyncServiceTrace> result(s.traces.begin(),s.traces.end());s.traces.clear();return result;
}
bool AsyncRuntime::drained() const {return impl_->served && !impl_->poisoned && impl_->order->idle() && impl_->backend.drained();}
}
