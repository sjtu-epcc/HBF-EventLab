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
    "advance","results_scan","reclaim","event_run"};
constexpr const char* counters[]={"split_fragments","prepare_globals","registered_globals","registered_fragments",
    "input_registered","input_backpressured","input_too_large","backpressured_fragments",
    "pump_queue_visits","pump_candidates","handoff_attempts","handoff_taken",
    "results_entries_visited","results_size_peak","reclaim_unit_visits",
    "reclaimed_work","reclaimed_units","reclaimed_requests"};
static_assert(sizeof(regions)/sizeof(*regions)==static_cast<std::size_t>(RuntimeRegion::Count));
static_assert(sizeof(counters)/sizeof(*counters)==static_cast<std::size_t>(RuntimeCounter::Count));
}
RuntimeProfile::RuntimeProfile(Clock clock) noexcept :clock_(clock?clock:steady_ns) {}
RuntimeScope::RuntimeScope(RuntimeProfile* profile,RuntimeRegion region) noexcept :profile_(profile),region_(region) {
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
    json<<"}}\n";out<<json.str();
    if(!out)throw std::runtime_error("runtime profile output failed");
}
} // namespace hbf_eventlab
