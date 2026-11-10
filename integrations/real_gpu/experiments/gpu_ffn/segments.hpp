#pragma once
// Native captures GPU activity windows; modeled runs consume the resulting table.
static std::ofstream segments;
static std::uint64_t activity_id=0, segment_id=0, window_start=0, window_activity=0;
static std::uint64_t fixed_segment_ps=0;
static bool window_open=false, capture_compute=false;
static json compute_rows;
static std::size_t compute_cursor=0;
static void load_compute_table(const json& config) {
    capture_compute=config.value("capture_compute",false);
    if(capture_compute) { need(!config.at("hbf").value("enabled",false),"Capture must be native"); return; }
    std::ifstream f(config.at("compute_table_path").get<std::string>());
    need(bool(f),"Cannot read compute table");json table;f>>table;
    need(table.at("header")==config.at("expected_compute_header"),"Compute header mismatch");
    need(table.at("header").at("frozen_G1")==true,"Compute table not frozen");
    compute_rows=table.at("segments");need(!compute_rows.empty(),"Empty compute table");
    for(std::size_t i=0;i<compute_rows.size();++i) {
        const auto& r=compute_rows.at(i);
        need(r.at("segment_id")==i+1,"Missing/duplicate/reordered compute segment");
        need(r.at("duration_ps").is_number_unsigned(),"Invalid integer compute duration");
    }
}
static void finish_compute_table() {
    need(!window_open,"Open compute window");
    need(capture_compute||compute_cursor==compute_rows.size(),"Unconsumed compute segments");
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
    need(std::string(kind)!="verification","FFN experiment uses logits, not snapshot exports");
    const auto count=activity_id-window_activity;
    std::uint64_t duration=0;
    if(!capture_compute) {
        need(compute_cursor<compute_rows.size(),"Extra GPU segment");
        const auto& expected=compute_rows.at(compute_cursor++);
        need(expected.at("kind")==kind&&expected.at("node_id")==node&&expected.at("step")==step.load(),"Compute segment boundary mismatch");
        duration=expected.at("duration_ps").get<std::uint64_t>();
        need((count>0)==(duration>0),"GPU activity coverage differs from compute table");
    }
    json row={{"segment_id",++segment_id},{"kind",kind},{"node_id",node},{"step",step.load()},
        {"start_ns",window_start},{"end_ns",end},{"first_activity_id",window_activity+1},
        {"activity_count",count},{"duration_ps",duration}};
    if(gate)row["advance"]=gate->advance_segment(duration);
    segments<<row.dump()<<'\n';need(bool(segments),"Segment record write failed");window_open=false;
}
