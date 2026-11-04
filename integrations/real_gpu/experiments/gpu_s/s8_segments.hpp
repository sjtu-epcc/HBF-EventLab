#pragma once
#include <limits>
#include <set>
#include <tuple>
// Application-only synchronization boundaries. All streams on the selected device
// complete before a dependency is submitted. GPU activity time is never inferred
// from CPU wall time. Empty windows and verification copies receive no model time.
static std::ofstream segments;
static std::uint64_t activity_id=0, segment_id=0, window_start=0, window_activity=0;
static std::uint64_t fixed_segment_ps=1000;
static bool window_open=false;
static json compute_rows;
static std::size_t compute_cursor=0;
static bool compute_uint64(const json& value) {
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<std::int64_t>()>=0);
}
static void load_compute_table(const json& config) {
    need(!window_open&&compute_cursor==0,"Cannot replace an active compute table");
    std::ifstream f(config.at("compute_table_path").get<std::string>());
    need(bool(f),"Cannot read qualified compute table");json table;f>>table;
    need(table.at("header").is_object(),"Invalid compute header");
    need(table.at("header")==config.at("expected_compute_header"),"Compute header mismatch");
    need(table.at("header").at("frozen_G1")==true,"Compute table not frozen");
    // The caller supplies the complete frozen trajectory, including an EOS-ready
    // step when present. A shorter run must never silently discard its suffix.
    compute_rows=table.at("segments");need(compute_rows.is_array(),"Invalid compute segments");
    std::set<std::tuple<std::uint64_t,std::string,std::uint64_t>> identities;
    std::uint64_t previous_step=0;
    for(std::size_t i=0;i<compute_rows.size();++i) {
        const auto& r=compute_rows.at(i);
        need(compute_uint64(r.at("segment_id"))&&r.at("segment_id")==i+1,"Missing/duplicate/reordered compute segment");
        need(compute_uint64(r.at("duration_ps")),"Invalid uint64 compute duration");
        need(r.at("kind").is_string(),"Invalid compute segment kind");
        const auto kind=r.at("kind").get<std::string>();
        need(kind=="gap"||kind=="node"||kind=="tail","Invalid compute segment kind");
        need(compute_uint64(r.at("node_id")),"Invalid compute node identity");
        need(compute_uint64(r.at("step")),"Invalid compute step identity");
        const auto owner=r.at("step").get<std::uint64_t>();
        need(owner<=static_cast<std::uint64_t>(std::numeric_limits<int>::max()),"Compute step out of range");
        need(i==0?owner==0:(owner==previous_step||owner==previous_step+1),"Missing/reordered compute step");
        previous_step=owner;
        need(identities.emplace(owner,kind,r.at("node_id").get<std::uint64_t>()).second,"Duplicate compute boundary");
    }
}
static void finish_compute_table() {
    need(!window_open,"Unclosed compute segment");
    need(compute_cursor==compute_rows.size(),"Unconsumed compute segments (requires the complete matching frozen trajectory)");
}

static void segment_begin() {
    need(!window_open,"Overlapping segment windows");
    window_start=stamp();window_activity=activity_id;window_open=true;
}
static void segment_end(const char* kind,std::uint64_t node) {
    need(window_open,"Missing segment start");
    need(cudaDeviceSynchronize()==cudaSuccess,"Segment GPU synchronization failed");
    const auto end=stamp();
    need(cuptiActivityFlushAll(0)==CUPTI_SUCCESS,"Segment CUPTI flush failed");
    std::lock_guard<std::mutex> guard(kernel_mutex);
    const bool verification=std::string(kind)=="verification";
    const auto count=activity_id-window_activity;
    std::uint64_t duration=0,table_id=0;
    if(!verification) {
        need(compute_cursor<compute_rows.size(),"Extra GPU segment outside compute table");
        const auto& expected=compute_rows.at(compute_cursor++);
        need(expected.at("kind")==kind&&expected.at("node_id")==node&&expected.at("step")==step.load(),"Compute segment boundary mismatch");
        duration=expected.at("duration_ps").get<std::uint64_t>();table_id=expected.at("segment_id");
        need((count>0)==(duration>0),"Actual GPU activity coverage differs from frozen table");
    }
    json row={{"segment_id",++segment_id},{"kind",kind},{"node_id",node},{"step",step.load()},
              {"start_ns",window_start},{"end_ns",end},{"first_activity_id",window_activity+1},
              {"activity_count",count},{"modeled",!verification&&count>0},{"duration_ps",duration}};
    row["table_segment_id"]=verification?json(nullptr):json(table_id);
    if(gate&&!verification&&count)row["advance"]=gate->advance_segment(duration);
    segments<<row.dump()<<'\n';need(bool(segments),"Segment record write failed");window_open=false;
}
