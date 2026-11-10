#pragma once
#include "hbf_eventlab/interfaces/statistics_sink.hpp"
#include <algorithm>
#include <map>
#include <stdexcept>

// Retain only live work; completed fragment/attempt history is never accumulated.
// The expected address interval is supplied by the external manifest, not stats.
class OnlineAudit final : public hbf_eventlab::IStatisticsSink {
    using Op=hbf_eventlab::Op;
    struct Global { std::uint64_t defined=0,finished=0; };
    std::map<std::uint64_t,Global> globals_;
    std::map<std::uint64_t,std::uint64_t> work_;
    std::map<std::uint64_t,Op> attempts_;
    bool active_=false;
    Op op_=Op::Read;
    std::uint64_t base_=0,bytes_=0,next_=0,defined_globals_=0,finished_globals_=0,finished_work_=0,last_finished_=0;
    std::uint64_t last_global_id_=0,last_work_id_=0;
    static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
    template<class T> void event(const T&) {}
    void event(const hbf_eventlab::StatGlobalDefined& e) {
        check(active_&&e.op==op_&&e.bytes==4096,"Unexpected Global type/size");
        check(e.id.value>last_global_id_,"Reused/reordered Global ID");last_global_id_=e.id.value;
        check(globals_.emplace(e.id.value,Global{}).second,"Duplicate live Global");++defined_globals_;
    }
    void event(const hbf_eventlab::StatWorkDefined& e) {
        auto& g=globals_.at(e.parent.value);
        check(e.id.value>last_work_id_,"Reused/reordered fragment ID");last_work_id_=e.id.value;
        check(next_<bytes_&&e.address.channel==0&&e.address.local.value==base_+next_,"Fragment address/coverage mismatch");
        check(op_==Op::Read?!e.content:e.content==std::optional<std::uint64_t>{1},"Fragment content identity mismatch");
        check(work_.emplace(e.id.value,e.parent.value).second,"Duplicate fragment");
        next_+=64;++g.defined;peak_work=std::max(peak_work,work_.size());
    }
    void event(const hbf_eventlab::StatWorkFinished& e) {
        auto it=work_.find(e.id.value);check(it!=work_.end(),"Unknown/duplicate fragment completion");
        ++globals_.at(it->second).finished;work_.erase(it);++finished_work_;
        if(!e.success)++failures;
    }
    void event(const hbf_eventlab::StatGlobalFinished& e) {
        const auto g=globals_.at(e.id.value);
        check(g.defined==64&&g.finished==64,"Incomplete Global fragment coverage");
        globals_.erase(e.id.value);++finished_globals_;last_finished_=std::max(last_finished_,e.at.value);
        if(!e.success)++failures;
    }
    void event(const hbf_eventlab::StatAttemptStarted& e) {
        check(attempts_.emplace(e.id.value,e.command.op).second,"Duplicate attempt");
        peak_attempts=std::max(peak_attempts,attempts_.size());
    }
    void event(const hbf_eventlab::StatAttemptAssessed& e) {
        check(attempts_.erase(e.id.value)==1,"Unknown attempt assessment");
        if(!e.success)++attempt_errors;
    }
public:
    std::uint64_t failures=0,attempt_errors=0,total_read_fragments=0,total_write_fragments=0;
    std::size_t peak_work=0,peak_attempts=0;
    void begin(Op op,std::uint64_t base,std::uint64_t bytes) {
        check(!active_&&globals_.empty()&&work_.empty()&&attempts_.empty(),"Previous parent not drained");
        check(bytes&&base%4096==0&&bytes%4096==0,"Expected interval must cover whole DLUs");
        op_=op;base_=base;bytes_=bytes;next_=defined_globals_=finished_globals_=finished_work_=last_finished_=0;active_=true;
    }
    void end(std::uint64_t registered,std::uint64_t finished,std::uint64_t completed) {
        check(active_&&globals_.empty()&&work_.empty()&&attempts_.empty(),"Parent left live audit state");
        check(next_==bytes_&&finished_work_==bytes_/64,"Missing/extra logical fragments");
        check(defined_globals_==bytes_/4096&&registered==defined_globals_&&finished==finished_globals_&&finished==registered,"Global count mismatch");
        check(completed==last_finished_,"Parent completion is not maximum child completion");
        check(!failures,"Logical access failed");
        if(op_==Op::Read)total_read_fragments+=finished_work_;else total_write_fragments+=finished_work_;
        active_=false;
    }
    void observe(const hbf_eventlab::StatisticsEvent& e) override {std::visit([this](const auto& x){event(x);},e);}
};
