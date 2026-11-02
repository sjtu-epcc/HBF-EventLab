#include "hbflab/live_runtime.hpp"
#include <cuda.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <string>

namespace hbflab {
struct LiveTimingService::Impl {
    const std::thread::id owner=std::this_thread::get_id();
    CUcontext context=nullptr;
    std::thread worker;
    std::mutex mutex;std::condition_variable cv;
    std::deque<std::function<void()>> commands;
    bool stopping=false,started=false,closed=false;
    std::exception_ptr error;
    std::unique_ptr<AsyncRuntime> runtime;
    std::uint32_t producers,launch_limit;
    hbf_eventlab::Geometry geometry;
    Impl(AsyncRuntimeOptions options,std::uint32_t slots,std::uint32_t launches,LiveWorkerHooks hooks):producers(slots),launch_limit(launches),geometry(options.backend.geometry) {
        if(!slots || slots>8192 || !launches || launches>256)throw std::invalid_argument("invalid live service limits");
        if(cuCtxGetCurrent(&context)!=CUDA_SUCCESS || !context)throw std::runtime_error("live service requires current CUDA context");
        std::promise<void> ready;auto wait=ready.get_future();
        worker=std::thread([this,options,hooks,&ready] {
            struct Scope {LiveWorkerHooks h;Scope(LiveWorkerHooks v):h(v){if(h.enter)h.enter();}~Scope(){if(h.leave)h.leave();}} scope(hooks);
            try {
                if(cuCtxSetCurrent(context)!=CUDA_SUCCESS)throw std::runtime_error("cannot bind service CUDA context");
                runtime=std::make_unique<AsyncRuntime>(options);ready.set_value();
            }catch(...){ready.set_exception(std::current_exception());return;}
            for(;;) {
                std::function<void()> command;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if(commands.empty() && (!started || closed || error || runtime->live_idle()))
                        cv.wait(lock,[this]{return stopping || !commands.empty();});
                    if(stopping)break;
                    if(!commands.empty()){command=std::move(commands.front());commands.pop_front();}
                }
                if(command)command();
                if(started && !closed && !error) {
                    try {pump();}catch(...){error=std::current_exception();}
                }
                std::this_thread::yield();
            }
            runtime.reset(); // signal terminal and drain kernels before unmapping slots
            cuCtxSetCurrent(nullptr);
        });
        try{wait.get();}catch(...){worker.join();throw;}
    }
    ~Impl() {
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;}cv.notify_one();
        if(worker.joinable())worker.join();
    }
    void current() const {
        CUcontext actual=nullptr;
        if(owner!=std::this_thread::get_id() || cuCtxGetCurrent(&actual)!=CUDA_SUCCESS || actual!=context)
            throw std::runtime_error("live service owner/context mismatch");
    }
    void pump() {
        if(error)std::rethrow_exception(error);
        const auto status=runtime->poll_live();
        if(status!=Status::Ok)throw std::runtime_error("live timing terminal status "+std::to_string(static_cast<unsigned>(status)));
    }
    template<class F> auto invoke(F f,bool allow_error=false) -> decltype(f()) {
        current();using R=decltype(f());
        auto task=std::make_shared<std::packaged_task<R()>>([this,f,allow_error] {
            if(error && !allow_error)std::rethrow_exception(error);
            return f();
        });
        auto result=task->get_future();
        {std::lock_guard<std::mutex> lock(mutex);
            if(stopping || commands.size()>=16)throw std::runtime_error("live command queue unavailable");
            commands.emplace_back([task]{(*task)();});}
        cv.notify_one();return result.get();
    }
    void sync() {
        if(!started || closed)throw std::runtime_error("live session is not running");
        while(!runtime->live_idle()){pump();std::this_thread::yield();}
    }
};
LiveTimingService::LiveTimingService(AsyncRuntimeOptions o,std::uint32_t p,std::uint32_t l,LiveWorkerHooks hooks):impl_(std::make_unique<Impl>(o,p,l,hooks)){}
LiveTimingService::~LiveTimingService()=default;
void LiveTimingService::register_readonly(const void* p,std::size_t n){auto& s=*impl_;s.invoke([&]{s.runtime->register_readonly(p,n);});}
void LiveTimingService::start(){auto& s=*impl_;s.invoke([&]{s.runtime->start_live(s.producers,s.launch_limit);s.started=true;});}
std::uint64_t LiveTimingService::launch(AsyncLaunchShape shape,const void* kernel,const std::vector<void*>& args,cudaStream_t stream,std::size_t shared) {
    auto& s=*impl_;
    return s.invoke([&] {
        if(!s.started || s.closed)throw std::runtime_error("launch outside live session");
        for(;;) {
            if(const auto id=s.runtime->launch_live(shape,kernel,args,stream,shared))return id;
            s.pump();std::this_thread::yield();
        }
    });
}
std::uint64_t LiveTimingService::launch_driver(AsyncLaunchShape shape,std::uintptr_t function,const std::vector<void*>& args,cudaStream_t stream) {
    auto& s=*impl_;return s.invoke([&] {
        if(!s.started || s.closed)throw std::runtime_error("driver launch outside live session");
        for(;;){if(const auto id=s.runtime->launch_driver_live(shape,function,args,stream))return id;s.pump();std::this_thread::yield();}
    });
}
void LiveTimingService::synchronize(){auto& s=*impl_;s.invoke([&]{s.sync();});}
void LiveTimingService::check_status(){auto& s=*impl_;s.invoke([&]{if(!s.started || s.closed)throw std::runtime_error("status outside live session");s.pump();});}
void LiveTimingService::finish(){auto& s=*impl_;s.invoke([&]{s.sync();s.runtime->finish_live();s.closed=true;});}
AsyncRuntimeStats LiveTimingService::statistics(){auto& s=*impl_;return s.invoke([&]{return s.runtime->statistics();});}
std::vector<AsyncServiceTrace> LiveTimingService::audit(){auto& s=*impl_;return s.invoke([&]{return s.runtime->audit();});}
std::vector<AsyncServiceTrace> LiveTimingService::take_audit(){auto& s=*impl_;return s.invoke([&]{return s.runtime->take_audit();});}
bool LiveTimingService::drained(){auto& s=*impl_;return s.invoke([&]{return s.runtime->drained();});}
std::uint64_t LiveTimingService::reserve_media(std::uint64_t bytes){auto& s=*impl_;return s.invoke([&]{return s.runtime->reserve_media(bytes);});}
ReadResult LiveTimingService::read_media(std::uint64_t logical,std::uint32_t bytes){auto& s=*impl_;return s.invoke([&]{return s.runtime->read_media(logical,bytes);});}
WriteResult LiveTimingService::write_media(std::uint64_t logical,const std::array<std::uint64_t,64>& tokens){auto& s=*impl_;return s.invoke([&]{return s.runtime->write_media(logical,tokens);});}
BackendStats LiveTimingService::media_statistics(){auto& s=*impl_;return s.invoke([&]{return s.runtime->statistics().backend;},true);}
std::uint64_t LiveTimingService::media_time_ps(){auto& s=*impl_;return s.invoke([&]{return s.runtime->statistics().model_now_ps;},true);}
MediaSample LiveTimingService::sample_media(){auto& s=*impl_;return s.invoke([&]{const auto stats=s.runtime->statistics();return MediaSample{stats.model_now_ps,stats.backend};},true);}
std::optional<hbf_eventlab::Geometry> LiveTimingService::media_geometry(){impl_->current();return impl_->geometry;}
void LiveTimingService::abort_media(){auto& s=*impl_;s.invoke([&]{s.runtime->abort();s.error=std::make_exception_ptr(std::runtime_error("shared capacity session aborted"));},true);}
std::vector<AsyncMediaTrace> LiveTimingService::take_media_audit(){auto& s=*impl_;return s.invoke([&]{return s.runtime->take_media_audit();});}

}
