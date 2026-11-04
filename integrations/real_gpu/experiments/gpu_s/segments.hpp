#pragma once
// Application-only synchronization boundaries. All streams on the selected device
// complete before a dependency is submitted. GPU activity time is never inferred
// from CPU wall time. Empty windows and verification copies receive no model time.
static std::ofstream segments;
static std::uint64_t activity_id=0, segment_id=0, window_start=0, window_activity=0;
static std::uint64_t fixed_segment_ps=1000;
static bool window_open=false;
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
    const auto duration=(!verification&&count)?fixed_segment_ps:0;
    json row={{"segment_id",++segment_id},{"kind",kind},{"node_id",node},{"step",step.load()},
              {"start_ns",window_start},{"end_ns",end},{"first_activity_id",window_activity+1},
              {"activity_count",count},{"modeled",!verification&&count>0},{"duration_ps",duration}};
    if(gate&&!verification&&count)row["advance"]=gate->advance_segment(duration);
    segments<<row.dump()<<'\n';need(bool(segments),"Segment record write failed");window_open=false;
}
