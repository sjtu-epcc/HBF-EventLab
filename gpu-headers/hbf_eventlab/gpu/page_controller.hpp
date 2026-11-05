#pragma once
#include "hbf_eventlab/host/workload.hpp"
#include "hbf_eventlab/host/host_driver.hpp"
#include "write_coverage.hpp"
#include <deque>
#include <bitset>

namespace hbf_eventlab {
struct GpuTraceRequest {
    GlobalRequest request;
    std::uint64_t gpu_cycle=0;
    std::uint32_t source=0;
    std::bitset<128> byte_mask;
};
struct GpuControllerConfiguration {
    Tick cycle{1000},write_deadline{10000000},read_window{};
    std::size_t issue_width=2,issue_limit=128,mshrs=16,write_buffers=8;
    bool merge_reads=true;
    bool sector_requests=false; // explicit opt-in for live 32B/masked requests
};
struct GpuControllerStatistics {
    std::uint64_t issued=0,completed=0,failed=0,mshr_hits=0,page_reads=0,page_writes=0;
    std::uint64_t requested_read_bytes=0,media_read_bytes=0,unique_write_bytes=0;
    std::uint64_t incomplete_writes=0,max_outstanding=0,max_mshrs=0;
    std::uint64_t issue_stalls=0,total_latency=0,last_completion=0,input_backpressure=0;
};
struct GpuRequestResult { bool success;Tick issued,completed;std::uint32_t source;std::string reason; };

// Online 64B read/page write conversion. HostDriver owns retries and replay.
class GpuPageController {
public:
    GpuPageController(HostDriver&,EventQueue&,std::vector<GpuTraceRequest>,GpuControllerConfiguration={});
    ~GpuPageController();
    void start();
    // Streaming dependencies may refer only to previously admitted source IDs.
    void enqueue(GpuTraceRequest);
    void set_result_observer(std::function<void(GlobalRequestId,const GpuRequestResult&)> f) { observer_=std::move(f); }
    bool drained() const;
    bool success() const {return drained()&&!stats_.failed;}
    std::string waiting_reason() const;
    const GpuControllerStatistics& statistics() const {return stats_;}
    const std::map<GlobalRequestId,GpuRequestResult>& results() const {return results_;}
private:
    struct Page {
        Op op;std::uint64_t address,bytes,content;std::optional<std::uint64_t> expected;
        std::vector<std::size_t> waiters;
        hbf_sim_import::hbf_write_coverage_t coverage{4096};
        std::optional<GlobalRequestId> backend;
        std::optional<EventId> timer;
        bool ready=false;
    };
    struct Request {GpuTraceRequest trace;bool issued=false,done=false;Tick at;
        std::size_t pending=0;bool failed=false;std::string reason;};
    using Key=std::pair<std::uint64_t,std::uint32_t>; // page, 64B unit; 64 means whole-page write
    EventId schedule(Tick,std::function<void()>);
    void validate_request(const GpuTraceRequest&) const;
    void cancel(std::optional<EventId>&);
    void wake();
    void pump();
    bool admit(std::size_t);
    void service_pages();
    void submit(Key,Page&);
    void backend_completed(const WorkResult&,Tick);
    void finish(std::size_t,bool,const std::string&);
    void flush_incomplete(Key,const std::string&);
    std::size_t mshr_count() const;
    Tick next_cycle() const;
    HostDriver& host_;EventQueue& events_;GpuControllerConfiguration config_;
    std::vector<Request> requests_;
    std::map<GlobalRequestId,std::size_t> index_;
    std::map<std::uint32_t,std::deque<std::size_t>> sources_;
    std::map<Key,Page> pages_;
    std::map<GlobalRequestId,Key> backend_;
    std::map<GlobalRequestId,GpuRequestResult> results_;
    std::set<EventId> events_owned_;
    std::optional<EventId> pump_event_;
    std::optional<Tick> last_issue_cycle_;
    std::size_t outstanding_=0;
    std::uint64_t next_backend_=0;
    bool started_=false;
    std::shared_ptr<bool> alive_=std::make_shared<bool>(true);
    GpuControllerStatistics stats_;
    std::function<void(GlobalRequestId,const GpuRequestResult&)> observer_;
};
} // namespace hbf_eventlab

