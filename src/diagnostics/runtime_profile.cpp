#include "hbf_eventlab/diagnostics/runtime_profile.hpp"
#include <algorithm>
#include <chrono>
#include <exception>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace hbf_eventlab {
namespace {
std::uint64_t steady_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
constexpr const char* regions[]={"split","prepare","register_input","admit_input","pump","receive",
    "advance","results_scan","reclaim","event_run","completion_check","wait_notify","wait_callback","read_wakeup","device_pump"};
constexpr const char* counters[]={"split_fragments","prepare_globals","registered_globals","registered_fragments",
    "input_registered","input_backpressured","input_too_large","backpressured_fragments",
    "pump_queue_visits","pump_candidates","handoff_attempts","handoff_taken",
    "results_entries_visited","results_size_peak","reclaim_unit_visits",
    "reclaimed_work","reclaimed_units","reclaimed_requests","read_arms","read_wakeups","read_rearms",
    "read_wake_resolved","read_result_blocked","waits_peak"};
constexpr const char* wait_names[]={"credit","block_progress","logical_readers","physical_users","zone",
    "dlu_progress","read_order","result_space","response_ready","ingress_space","request_link","bank","cache_space","sense_order",
    "physical_state","zone_users"};
static_assert(sizeof(wait_names)/sizeof(*wait_names)==static_cast<std::size_t>(WaitReason::Count));
static_assert(sizeof(regions)/sizeof(*regions)==static_cast<std::size_t>(RuntimeRegion::Count));
static_assert(sizeof(counters)/sizeof(*counters)==static_cast<std::size_t>(RuntimeCounter::Count));
}
RuntimeProfile::RuntimeProfile(Clock clock) noexcept :clock_(clock?clock:steady_ns) {}
RuntimeScope::RuntimeScope(RuntimeProfile* profile,RuntimeRegion region,WaitReason trigger) noexcept :profile_(profile),region_(region),trigger_(trigger) {
    if(!profile_)return;
    exceptions_=std::uncaught_exceptions();parent_=profile_->active_;
    start_=profile_->clock_();profile_->active_=this;
}
RuntimeScope::~RuntimeScope() noexcept {
    if(!profile_)return;
    const auto elapsed=profile_->clock_()-start_;
    auto& m=profile_->regions_[static_cast<std::size_t>(region_)];
    ++m.calls;m.unwound_calls+=std::uncaught_exceptions()>exceptions_;
    m.inclusive_ns+=elapsed;m.self_ns+=elapsed-std::min(elapsed,children_);m.max_ns=std::max(m.max_ns,elapsed);
    if(trigger_!=WaitReason::Count)profile_->wait(trigger_).callback_ns+=elapsed;
    profile_->active_=parent_;
    if(parent_)parent_->children_+=elapsed;else profile_->root_ns_+=elapsed;
}
void RuntimeProfile::write_json(std::ostream& out) const {
    if(active_)throw std::logic_error("runtime profile export requires no active scope");
    std::ostringstream json;json.imbue(std::locale::classic());
    json<<"{\"schema_version\":1,\"clock\":\""<<(clock_==steady_ns?"steady_clock":"custom")<<"\",\"unit\":\"program_wall_ns\","
        <<"\"self_excludes\":\"instrumented_children_only\",\"covered_root_ns\":"<<root_ns_<<",\"regions\":{";
    for(std::size_t i=0;i<regions_.size();++i) {
        const auto& m=regions_[i];if(i)json<<',';
        json<<'"'<<regions[i]<<"\":{\"calls\":"<<m.calls<<",\"unwound_calls\":"<<m.unwound_calls
            <<",\"inclusive_ns\":"<<m.inclusive_ns<<",\"self_ns\":"<<m.self_ns<<",\"max_ns\":"<<m.max_ns<<'}';
    }
    json<<"},\"counters\":{";
    for(std::size_t i=0;i<counters_.size();++i) {if(i)json<<',';json<<'"'<<counters[i]<<"\":"<<counters_[i];}
    json<<"},\"wait_reasons\":{";
    for(std::size_t i=0;i<waits_.size();++i) {
        const auto& w=waits_[i];if(i)json<<',';
        json<<'"'<<wait_names[i]<<"\":{\"notifications\":"<<w.notifications<<",\"subscriber_hits\":"<<w.subscriber_hits
            <<",\"queued\":"<<w.queued<<",\"callbacks\":"<<w.callbacks<<",\"cancelled\":"<<w.cancelled
            <<",\"revision_wakes\":"<<w.revision_wakes<<",\"callback_ns\":"<<w.callback_ns<<'}';
    }
    json<<"}}\n";out<<json.str();
    if(!out)throw std::runtime_error("runtime profile output failed");
}
} // namespace hbf_eventlab
