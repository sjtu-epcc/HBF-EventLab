#include "hbf_eventlab/gpu/page_controller.hpp"
#include <algorithm>

namespace hbf_eventlab {
GpuPageController::GpuPageController(HostDriver& host,EventQueue& events,std::vector<GpuTraceRequest> trace,GpuControllerConfiguration config)
    :host_(host),events_(events),config_(config) {
    if(!config.cycle.value||!config.issue_width||!config.issue_limit||!config.mshrs||!config.write_buffers||!config.write_deadline.value)
        throw std::invalid_argument("invalid GPU controller capacities/timing");
    for(auto& item:trace) {
        const auto& r=item.request;
        validate_request(item);
        if(!index_.emplace(r.id,requests_.size()).second)throw std::invalid_argument("duplicate GPU request ID");
        sources_[item.source].push_back(requests_.size());requests_.push_back({std::move(item),false,false,{}});
    }
    // Validate dependency acyclicity iteratively before any device activity.
    std::vector<std::size_t> degree(requests_.size());std::vector<std::vector<std::size_t>> followers(requests_.size());
    for(std::size_t i=0;i<requests_.size();++i)for(auto id:requests_[i].trace.request.after) {
        if(!index_.count(id))throw std::invalid_argument("unknown GPU dependency");
        ++degree[i];followers[index_.at(id)].push_back(i);
    }
    std::deque<std::size_t> ready;for(std::size_t i=0;i<degree.size();++i)if(!degree[i])ready.push_back(i);
    std::size_t visited=0;while(!ready.empty()){auto i=ready.front();ready.pop_front();++visited;for(auto j:followers[i])if(!--degree[j])ready.push_back(j);}
    if(visited!=requests_.size())throw std::invalid_argument("cyclic GPU dependencies");
    host_.set_completion_handler([this,alive=alive_](const GlobalCompletion& result){
        if(*alive)backend_completed(result.result,result.at);
    });
    host_.set_input_capacity_handler([this,alive=alive_](InputCapacity){
        if(*alive){service_pages();wake();}
    });
}
void GpuPageController::validate_request(const GpuTraceRequest& item) const {
    const auto& r=item.request;
    if(r.op!=Op::Read&&r.op!=Op::Write)throw std::invalid_argument("invalid GPU operation");
    if(!r.bytes||r.bytes>4096||r.address.value/4096!=checked_add(r.address.value,r.bytes-1)/4096)
        throw std::invalid_argument("GPU request must be contained in one 4KiB page");
    if(!config_.sector_requests&&(r.bytes%64||r.address.value%64||item.byte_mask.any()))
        throw std::invalid_argument("GPU request must be 64B aligned without a live byte mask");
    if(item.byte_mask.any()&&r.bytes>128)throw std::invalid_argument("masked access exceeds 128B window");
    if(r.op==Op::Read && ((r.address.value%4096+r.bytes-1)/64-(r.address.value%4096)/64+1)>config_.mshrs)
        throw std::invalid_argument("GPU read spans more 64B units than available MSHRs");
}
void GpuPageController::enqueue(GpuTraceRequest item) {
    if(!started_)throw std::logic_error("streaming GPU must be started");
    validate_request(item);
    const auto& r=item.request;
    if(index_.count(r.id))throw std::invalid_argument("duplicate GPU request ID");
    if(r.not_before!=events_.now())throw std::invalid_argument("live GPU request must be ready now");
    for(auto id:r.after)if(!index_.count(id))throw std::invalid_argument("live dependency must already exist");
    const auto i=requests_.size();index_.emplace(r.id,i);sources_[item.source].push_back(i);
    requests_.push_back({std::move(item),false,false,{}});wake();
}
GpuPageController::~GpuPageController() {
    *alive_=false;for(auto id:events_owned_)events_.cancel(id);
}
EventId GpuPageController::schedule(Tick at,std::function<void()> f) {
    auto id=std::make_shared<EventId>();
    *id=events_.schedule_at(at,EventPhase::Arrival,[this,id,f=std::move(f)]{events_owned_.erase(*id);f();});
    events_owned_.insert(*id);return *id;
}
void GpuPageController::cancel(std::optional<EventId>& id) {
    if(id){events_.cancel(*id);events_owned_.erase(*id);id.reset();}
}
Tick GpuPageController::next_cycle() const {
    const auto now=events_.now().value,period=config_.cycle.value;
    auto cycle=now/period+(now%period!=0);
    if(last_issue_cycle_&&cycle<=last_issue_cycle_->value/period)cycle=last_issue_cycle_->value/period+1;
    return Tick{checked_mul(cycle,period)};
}
void GpuPageController::start(){if(started_)throw std::logic_error("GPU already started");started_=true;wake();}
void GpuPageController::wake(){if(started_&&!pump_event_&&!drained())pump_event_=schedule(next_cycle(),[this]{pump_event_.reset();pump();});}
std::size_t GpuPageController::mshr_count() const {
    std::size_t count=0;for(const auto& p:pages_)count+=p.second.op==Op::Read||p.second.backend.has_value();return count;
}
void GpuPageController::finish(std::size_t i,bool success,const std::string& reason) {
    auto& r=requests_.at(i);if(r.done)throw std::logic_error("duplicate GPU completion");
    r.done=true;++stats_.completed;if(!success)++stats_.failed;
    if(r.issued){if(!outstanding_)throw std::logic_error("GPU outstanding underflow");--outstanding_;}
    const auto now=events_.now();const auto ready=r.trace.request.not_before;
    stats_.total_latency=checked_add(stats_.total_latency,now<ready?0:now.value-ready.value);
    stats_.last_completion=now.value;
    const auto id=r.trace.request.id;
    auto result=results_.emplace(id,GpuRequestResult{success,r.at,now,r.trace.source,reason});
    if(observer_)observer_(id,result.first->second);
}
void GpuPageController::flush_incomplete(Key key,const std::string& reason) {
    const auto found=pages_.find(key);if(found==pages_.end()||found->second.backend||found->second.coverage.full())return;
    cancel(found->second.timer);auto waiters=found->second.waiters;pages_.erase(found);++stats_.incomplete_writes;
    for(auto i:waiters)finish(i,false,reason);
    wake();
}
bool GpuPageController::admit(std::size_t i) {
    auto& record=requests_.at(i);const auto& r=record.trace.request;
    if(events_.now()<r.not_before)return false;
    for(auto dependency:r.after){const auto& dep=requests_.at(index_.at(dependency));if(!dep.done)return false;
        if(!results_.at(dependency).success){finish(i,false,"DependencyFailed");return true;}}
    if(outstanding_>=config_.issue_limit)return false;
    const auto page_number=r.address.value/4096;
    const Key write_key{page_number,64};
    std::vector<Key> keys;
    if(r.op==Op::Read) {
        const auto write=pages_.find(write_key);
        if(write!=pages_.end()) {
            if(!write->second.backend&&!write->second.coverage.full())
                flush_incomplete(write_key,"ReadAfterIncompleteWrite");
            return false;
        }
        const auto first=static_cast<std::uint32_t>((r.address.value%4096)/64);
        const auto last=static_cast<std::uint32_t>(((r.address.value%4096)+r.bytes-1)/64);
        std::size_t missing=0;
        for(auto unit=first;unit<=last;++unit) {
            const Key key{page_number,unit};keys.push_back(key);
            const auto found=pages_.find(key);
            if(found==pages_.end())++missing;
            else if(!config_.merge_reads||found->second.expected!=r.expected_content)return false;
        }
        if(missing>config_.mshrs-mshr_count())return false;
    } else {
        // Writes still assemble complete 4KiB pages. Do not race a read unit
        // on the same page, even when their byte ranges do not overlap.
        const auto first=pages_.lower_bound(Key{page_number,0});
        if(first!=pages_.end()&&first->first.first==page_number&&first->first!=write_key)return false;
        const auto found=pages_.find(write_key);
        if(found==pages_.end()) {
            std::size_t buffered=0;for(const auto& p:pages_)buffered+=p.second.op==Op::Write&&!p.second.backend;
            if(buffered>=config_.write_buffers)return false;
        } else if(found->second.backend||found->second.content!=r.content||found->second.ready)return false;
        keys.push_back(write_key);
    }
    for(const auto key:keys) {
        auto found=pages_.find(key);
        if(found==pages_.end()) {
            const auto address=r.op==Op::Read?key.first*4096+key.second*64:key.first*4096;
            const auto bytes=r.op==Op::Read?64:4096;
            Page page{r.op,address,bytes,r.content,r.expected_content,{},hbf_sim_import::hbf_write_coverage_t{4096},{},{},false};
            found=pages_.emplace(key,std::move(page)).first;
            if(r.op==Op::Write)found->second.timer=schedule(Tick{checked_add(events_.now().value,config_.write_deadline.value)},
                [this,key]{flush_incomplete(key,"IncompleteWriteDeadline");});
            else if(config_.read_window.value)found->second.timer=schedule(Tick{checked_add(events_.now().value,config_.read_window.value)},
                [this,key]{auto it=pages_.find(key);if(it!=pages_.end()){it->second.timer.reset();it->second.ready=true;service_pages();}});
            else found->second.ready=true;
        } else if(r.op==Op::Read)++stats_.mshr_hits;
        found->second.waiters.push_back(i);
    }
    record.pending=keys.size();record.issued=true;record.at=events_.now();++outstanding_;++stats_.issued;
    if(r.op==Op::Read)stats_.requested_read_bytes=checked_add(stats_.requested_read_bytes,r.bytes);
    else {
        auto& page=pages_.at(write_key);
        unsigned added=0;
        const auto& mask=record.trace.byte_mask;
        const bool valid=mask.any()?page.coverage.add_mask((r.address.value-r.address.value%128)%4096,mask,&added)
            :page.coverage.add(r.address.value%4096,static_cast<unsigned>(r.bytes),&added);
        if(!valid)throw std::logic_error("write coverage overflow");
        stats_.unique_write_bytes=checked_add(stats_.unique_write_bytes,added);
        if(page.coverage.full()){page.ready=true;cancel(page.timer);}
    }
    stats_.max_outstanding=std::max<std::uint64_t>(stats_.max_outstanding,outstanding_);
    stats_.max_mshrs=std::max<std::uint64_t>(stats_.max_mshrs,mshr_count());
    return true;
}
void GpuPageController::submit(Key key,Page& page) {
    const GlobalRequestId id{next_backend_+1};
    GlobalRequest r{id,page.op,GlobalByteAddress{page.address},page.bytes,page.content,{}, {},events_.now(),page.expected};
    if(page.op==Op::Write)r.expected_content.reset();
    const auto admission=host_.try_submit(r);
    if(admission.disposition==InputDisposition::TooLarge)
        throw std::invalid_argument("GPU page exceeds Host resident input budget");
    if(admission.disposition==InputDisposition::Backpressured){++stats_.input_backpressure;return;}
    ++next_backend_;page.backend=id;backend_.emplace(id,key);
    if(page.op==Op::Write)++stats_.page_writes;
    else{++stats_.page_reads;stats_.media_read_bytes+=page.bytes;}
    stats_.max_mshrs=std::max<std::uint64_t>(stats_.max_mshrs,mshr_count());
}
void GpuPageController::service_pages() {
    for(auto& item:pages_) {
        auto& page=item.second;
        if(!page.ready||page.backend)continue;
        if(page.op==Op::Write&&mshr_count()>=config_.mshrs)continue;
        submit(item.first,page);
    }
}
void GpuPageController::backend_completed(const WorkResult& result,Tick) {
    const auto it=backend_.find(result.id);if(it==backend_.end())throw std::logic_error("unknown GPU backend completion");
    const auto key=it->second;auto page=pages_.find(key);
    if(page==pages_.end())throw std::logic_error("missing GPU page MSHR");
    auto waiters=page->second.waiters;cancel(page->second.timer);pages_.erase(page);backend_.erase(it);
    for(auto i:waiters) {
        auto& request=requests_.at(i);
        if(!request.pending)throw std::logic_error("GPU read unit completion underflow");
        if(result.state!=LogicalResult::Success){request.failed=true;request.reason=result.reason;}
        if(!--request.pending)finish(i,!request.failed,request.reason);
    }
    service_pages();wake();
}
void GpuPageController::pump() {
    std::size_t issued=0;bool progress;
    do {
        progress=false;
        for(auto& source:sources_) {
            auto& queue=source.second;while(!queue.empty()&&requests_[queue.front()].done)queue.pop_front();
            if(queue.empty()||issued>=config_.issue_width)continue;
            const auto i=queue.front();
            if(admit(i)){queue.pop_front();++issued;progress=true;}
        }
    }while(progress&&issued<config_.issue_width);
    if(issued)last_issue_cycle_=events_.now();else if(!drained())++stats_.issue_stalls;
    service_pages();
    if(drained())return;
    if(issued==config_.issue_width){wake();return;}
    std::optional<Tick> release;
    for(const auto& source:sources_)if(!source.second.empty()) {
        const auto at=requests_[source.second.front()].trace.request.not_before;
        if(events_.now()<at&&(!release||at<*release))release=at;
    }
    if(release&&!pump_event_)pump_event_=schedule(*release,[this]{pump_event_.reset();wake();});
    // Resource/dependency stalls wake only on actual completions/deadlines.
}
bool GpuPageController::drained() const {return stats_.completed==requests_.size()&&!outstanding_&&pages_.empty()&&backend_.empty()&&events_owned_.empty();}
std::string GpuPageController::waiting_reason() const {
    if(!started_)return "GpuNotStarted";
    if(outstanding_>=config_.issue_limit)return "GpuIssueLimit";
    if(mshr_count()>=config_.mshrs)return "GpuMshrLimit";
    if(!pages_.empty())return "GpuWaitingForPageCompletionOrWriteCoverage";
    return "GpuWaitingForReleaseOrDependencies";
}
} // namespace hbf_eventlab

