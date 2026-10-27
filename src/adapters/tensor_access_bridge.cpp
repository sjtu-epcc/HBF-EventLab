#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include <algorithm>

namespace hbf_eventlab {
namespace {
struct ExecutionScope {bool& flag;explicit ExecutionScope(bool& f):flag(f){flag=true;}~ExecutionScope(){flag=false;}};
}
TensorAccessBridge::TensorAccessBridge(ResolvedSimulationConfiguration c,std::vector<TensorRegion> regions,
                                     TensorBridgeConfiguration b,IStatisticsSink* sink,RuntimeProfile* profile)
    :session_(std::move(c),sink,profile),config_(b) {
    if(!b.global_chunk_bytes||b.global_chunk_bytes%host_unit_bytes)
        throw std::invalid_argument("bridge Global chunk must be a positive multiple of 64B");
    const auto& config=session_.configuration().values();const auto& g=config.device.geometry;
    const auto end=checked_add(config.address.global_base.value,checked_mul(g.channel_bytes(),g.host_channels));
    RoundRobinInterleaver interleaver(config.address.global_base,g.host_channels,g.channel_bytes(),config.address.interleave_bytes);
    // Check immutable identities using only the public static logical address view.
    // Visit stripe/page boundaries, not every byte, and never inspect dynamic media.
    std::map<LogicalBlockKey,std::uint64_t> contents;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> intervals;
    for(auto& r:regions) {
        if(r.name.empty()||!r.bytes||r.base.value%host_unit_bytes||r.bytes%host_unit_bytes||r.base<config.address.global_base||checked_add(r.base.value,r.bytes)>end)
            throw std::invalid_argument("invalid tensor Global range");
        if(!tensors_.emplace(r.name,r).second)throw std::invalid_argument("duplicate tensor name");
        intervals.push_back({r.base.value,r.base.value+r.bytes});
        for(auto at=r.base.value;at<r.base.value+r.bytes;) {
            const auto local=interleaver.map(GlobalByteAddress{at});
            const auto logical=session_.device().layout().logical_view(local);
            const auto found=contents.emplace(logical.block,r.content);
            if(!found.second&&found.first->second!=r.content)throw std::invalid_argument("tensor contents conflict in one logical block");
            const auto stripe_left=config.address.interleave_bytes-(at-config.address.global_base.value)%config.address.interleave_bytes;
            at+=std::min({stripe_left,dlu_bytes-local.local.value%dlu_bytes,r.base.value+r.bytes-at});
        }
    }
    std::sort(intervals.begin(),intervals.end());
    for(std::size_t i=1;i<intervals.size();++i)
        if(intervals[i].first<intervals[i-1].second)throw std::invalid_argument("overlapping tensor Global ranges");
    session_.set_completion_handler([this](const auto& e){completed(e);});
    session_.set_input_capacity_handler([this](InputCapacity){if(active_&&!active_->result.complete&&!closed_)pump(true);});
}
void TensorAccessBridge::set_trace(std::function<void(const TensorBridgeTrace&)> trace) {
    if(executing_)throw std::logic_error("bridge callback is not reentrant");trace_=std::move(trace);
}
void TensorAccessBridge::emit(const char* action,GlobalRequestId id) {
    if(trace_)trace_({action,active_->result.id,id,session_.now()});
}
TensorAccessResult TensorAccessBridge::initialize(const std::string& name) {
    if(initialized_.count(name))throw std::logic_error("tensor initialization already attempted");
    const auto& r=tensors_.at(name);
    auto result=transfer(r,Op::Write,0,r.bytes,Tick{});
    initialized_.insert(name);return result;
}
TensorAccessResult TensorAccessBridge::access(const std::string& name,std::uint64_t offset,std::uint64_t bytes,Tick compute) {
    return transfer(tensors_.at(name),Op::Read,offset,bytes,compute);
}
TensorComputeResult TensorAccessBridge::advance_compute(Tick duration) {
    if(executing_||closed_||stopped_)throw std::logic_error("bridge is busy, stopped or closed");
    const auto start=session_.now();
    const Tick target{checked_add(start.value,duration.value)};
    if(!duration.value)return {start,start,true,RunStatus::Paused,{}};
    ExecutionScope execution(executing_);
    try {
        const auto report=session_.advance_before(target);
        const bool okay=report.run.at==target && (report.run.status==RunStatus::Paused||report.run.status==RunStatus::WaitingForInput);
        if(!okay)stopped_=true;
        return {start,report.run.at,okay,report.run.status,report.run.explanation};
    } catch(...) {stopped_=true;throw;}
}
TensorAccessResult TensorAccessBridge::transfer(const TensorRegion& tensor,Op op,std::uint64_t offset,std::uint64_t bytes,Tick compute) {
    if(executing_||closed_||stopped_)throw std::logic_error("bridge is busy, stopped or closed");
    if(!bytes||bytes%host_unit_bytes||offset%host_unit_bytes||offset>tensor.bytes||bytes>tensor.bytes-offset)
        throw std::invalid_argument("tensor access must be an aligned in-range interval");
    const auto capacity=session_.input_capacity();
    if(capacity.limit&&std::min(bytes,config_.global_chunk_bytes)/host_unit_bytes>capacity.limit)
        throw std::invalid_argument("bridge Global chunk exceeds total resident budget; choose a smaller chunk");
    const auto arrival=Tick{checked_add(session_.now().value,compute.value)};
    ExecutionScope execution(executing_);
    active_=std::make_unique<Active>(Active{{},GlobalByteAddress{checked_add(tensor.base.value,offset)},bytes,tensor.content});
    active_->result.id=next_access_=checked_add(next_access_,1);active_->result.op=op;
    active_->result.offered=arrival;active_->result.unsubmitted_bytes=bytes;
    try {
        if(compute.value) {
            emit("compute-start");
            const auto report=session_.advance_before(arrival);
            if(session_.now()!=arrival||(report.run.status!=RunStatus::Paused&&report.run.status!=RunStatus::WaitingForInput)) {
                active_->result.stop=report.run.status;active_->result.reason=report.run.explanation;stopped_=true;return active_->result;
            }
        }
        active_->input_open=true;emit("access-offered");
        if(session_.accepts_at_floor())pump(false);
        else session_.resume_feedback([this]{pump(true);}); // zero compute: a causal event, not +1 Tick
        while(!active_->result.complete) {
            const auto report=session_.advance_until_yield();
            if(report.feedback_yield&&active_->result.complete)break;
            active_->result.success=false;active_->result.stop=report.run.status;active_->result.reason=report.run.explanation;
            stopped_=true;return active_->result;
        }
    } catch(const std::invalid_argument& e) {
        stopped_=true;active_->failed=true;active_->result.success=false;active_->result.reason=e.what();
        active_->result.stop=RunStatus::ModelError;return active_->result;
    } catch(const std::exception& e) {
        stopped_=true;active_->failed=true;active_->result.success=false;active_->result.reason=e.what();
        active_->result.stop=RunStatus::ImplementationError;return active_->result;
    }
    stopped_=!active_->result.success;return active_->result;
}
void TensorAccessBridge::pump(bool feedback) {
    auto& a=*active_;if(a.result.complete||!a.input_open)return;
    while(!a.failed&&!closed_&&a.next<a.bytes) {
        const auto size=std::min(config_.global_chunk_bytes,a.bytes-a.next);
        const auto capacity=session_.input_capacity();
        // A notification is progress, not a promise this atomic chunk fits.
        // Do not revalidate/retry on every reclaimed 64B fragment.
        if(a.waiting_fragments&&capacity.limit&&*a.waiting_fragments>capacity.limit-capacity.resident)return;
        const GlobalRequestId id{checked_add(next_global_,1)};
        GlobalRequest request{id,a.result.op,GlobalByteAddress{a.base.value+a.next},size,a.content};
        request.not_before=session_.now();request.offered_at=a.result.offered;
        if(request.op==Op::Read)request.expected_content=a.content;
        InputAdmission result;
        try {result=feedback?session_.try_submit_feedback(request):session_.try_submit(request);}
        catch(const std::invalid_argument& e) {
            // A prior version may already have aborted before its whole Global
            // completion arrives. Stop the source suffix and drain registered
            // children; never turn this Host input rejection into a Device error.
            a.failed=true;a.result.reason=std::string("Host input contract: ")+e.what();
            a.result.stop=RunStatus::ModelError;break;
        }
        if(result.disposition==InputDisposition::TooLarge)throw std::logic_error("validated bridge chunk no longer fits total budget");
        if(result.disposition==InputDisposition::Backpressured) {a.waiting_fragments=size/host_unit_bytes;++a.result.capacity_rejections;emit("input-backpressured",id);return;}
        a.waiting_fragments.reset();
        next_global_=id.value;a.outstanding.insert(id);++a.result.registered_globals;a.next+=size;
        a.result.unsubmitted_bytes=a.bytes-a.next;emit("global-registered",id);
    }
    check_complete(feedback);
}
void TensorAccessBridge::completed(const GlobalCompletion& e) {
    if(!active_||!active_->outstanding.erase(e.result.id))throw std::logic_error("unknown bridge Global completion");
    auto& a=*active_;++a.result.finished_globals;emit("global-finished",e.result.id);
    if(e.result.state!=LogicalResult::Success) {a.failed=true;if(a.result.reason.empty())a.result.reason=e.result.reason;}
    if(!a.result.completed||*a.result.completed<e.at)a.result.completed=e.at;
    if(!closed_)pump(true);else check_complete();
}
void TensorAccessBridge::check_complete(bool feedback) {
    auto& a=*active_;
    if(a.result.complete||!a.outstanding.empty()||(!a.failed&&a.next<a.bytes))return;
    a.result.complete=true;a.result.success=!a.failed&&a.next==a.bytes;
    if(!a.result.completed)a.result.completed=session_.now();
    if(a.result.stop!=RunStatus::ModelError)a.result.stop=RunStatus::Completed;
    emit("access-finished");
    if(!closed_&&feedback)session_.request_yield();
}
SessionReport TensorAccessBridge::finish() {
    if(executing_)throw std::logic_error("bridge finish is not reentrant");
    ExecutionScope execution(executing_);closed_=true;
    if(active_&&!active_->result.complete)active_->failed=true; // explicit abandonment of unregistered work
    session_.close_input();return session_.run_to_completion();
}
} // namespace hbf_eventlab
