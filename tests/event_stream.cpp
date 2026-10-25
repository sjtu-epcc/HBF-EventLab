#include "hbf_eventlab/statistics/event_stream.hpp"
#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "hbf_eventlab/statistics/collector.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
using namespace hbf_eventlab;
namespace {
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("missing rejection");}
StatGlobalDefined defined(std::uint64_t id=1){return {GlobalRequestId{id},Op::Read,64,Tick{9}};}
void threshold(){
    std::ostringstream reference;StatisticsEventStream measure(reference,1);measure.observe(defined());const auto bytes=reference.str().size();
    std::ostringstream exact;StatisticsEventStream writer(exact,bytes);writer.observe(defined());
    check(exact.str()==reference.str()&&!writer.buffered_bytes(),"exact byte threshold flushes");
    std::ostringstream delayed;StatisticsEventStream buffer(delayed,bytes+1);buffer.observe(defined());
    check(delayed.str().empty()&&buffer.buffered_bytes()==bytes,"below byte threshold remains buffered");buffer.flush();
    check(delayed.str()==reference.str()&&!buffer.buffered_bytes(),"explicit flush");
}
void large_record(){
    std::ostringstream out;StatisticsEventStream stream(out,16);
    StatReadWindowReleased e{0,{},ReadReleaseReason::Threshold,Tick{1},Tick{10},Tick{5}};
    for(unsigned i=0;i<1000;++i)e.members.push_back(TransferAttemptId{i+1});
    stream.observe(e);check(!stream.buffered_bytes()&&stream.peak_buffered_bytes()<=16,"large record bypasses buffer");
    const auto text=out.str();check(text.back()=='\n'&&std::count(text.begin(),text.end(),'\n')==1,"large record is one complete JSON line");
    check(text.find("1000]")!=std::string::npos,"large membership not truncated");
}
void finish(){
    std::ostringstream out;StatisticsEventStream stream(out,65536);stream.observe(defined());check(out.str().empty(),"buffered before finish");
    stream.finish(Tick{10},"Paused");auto text=out.str();stream.finish(Tick{10},"Paused");check(text==out.str(),"finish idempotent");
    check(text.find("StreamEnd")!=std::string::npos&&stream.records()==2&&!stream.buffered_bytes(),"terminal marker and flush");
    rejects([&]{stream.observe(defined(2));});
}
void escaping(){
    std::ostringstream out;out<<std::hex;StatisticsEventStream stream(out,1);
    stream.observe(StatGlobalFinished{GlobalRequestId{16},Tick{32},false,"quote\" slash\\\n\t\x01"});stream.finish(Tick{32},"end");
    const auto s=out.str();check(s.find("\"id\":16")!=std::string::npos&&s.find("\"at\":32")!=std::string::npos,"caller format cannot alter IDs");
    check(s.find("quote\\\" slash\\\\\\u000a\\u0009\\u0001")!=std::string::npos,"JSON control escaping");
}
struct FailingBuffer:std::stringbuf {
    bool fail=false;
    std::streamsize xsputn(const char* s,std::streamsize n) override {return fail?0:std::stringbuf::xsputn(s,n);}
};
void write_failure(){
    FailingBuffer storage;std::ostream out(&storage);StatisticsEventStream stream(out,4096);stream.observe(defined());
    const auto buffered=stream.buffered_bytes();storage.fail=true;rejects([&]{stream.flush();});
    check(stream.buffered_bytes()==buffered,"failed write never discards pending bytes");storage.fail=false;out.clear();
    rejects([&]{stream.observe(defined(2));});rejects([&]{stream.flush();});
}
struct BadFlush:std::stringbuf {int sync() override{return -1;}};
void flush_failure(){BadFlush storage;std::ostream out(&storage);StatisticsEventStream stream(out,4096);stream.observe(defined());rejects([&]{stream.flush();});check(stream.buffered_bytes()>0,"flush failure retains buffer, no duplicate retry");}
void invalid(){std::ostringstream out;rejects([&]{StatisticsEventStream stream(out,0);});out.setstate(std::ios::badbit);rejects([&]{StatisticsEventStream stream(out);});rejects([&]{StatisticsFanout f({nullptr});});}
struct Result {std::string stream,json;Tick at;std::uint64_t events;};
Result run(std::size_t threshold,bool observe=true,bool fail=false) {
    std::ostringstream text,json;StatisticsEventStream stream(text,threshold);StatisticsCollector stats;StatisticsFanout fan({&stream,&stats});
    SimulationConfiguration c;c.device.geometry.host_channels=1;c.host.max_resident_fragments=128;if(fail)c.faults.fail_sequence={0};
    SimulationSession s(resolve_configuration(c),observe?&fan:nullptr);
    GlobalRequest w{GlobalRequestId{1},Op::Write,GlobalByteAddress{},4096,1};
    GlobalRequest r{GlobalRequestId{2},Op::Read,GlobalByteAddress{},4096};r.after={GlobalRequestId{1}};r.expected_content=1;
    s.submit(std::vector<GlobalRequest>{w,r});s.close_input();auto end=s.run_to_completion();check(end.run.status==RunStatus::Completed&&end.run.workload_success,"real workload completed");
    stream.finish(s.now(),"Completed");if(observe)stats.write_json(json,s.now(),"Completed");return {text.str(),json.str(),s.now(),end.run.events};
}
void observational(){auto small=run(97),large=run(65536),off=run(97,false);check(small.stream==large.stream&&small.json==large.json,"flush thresholds don't alter records or statistics");check(small.at==off.at&&small.events==off.events,"observation does not add simulation events");}
void replay(){auto a=run(97,true,true),b=run(65536,true,true);check(a.stream==b.stream&&a.json==b.json,"replay across many flushes identical");check(a.stream.find("\"replay\":true")!=std::string::npos,"replay identity exported");}
void partial(){
    std::ostringstream out;StatisticsEventStream stream(out,97);SimulationConfiguration c;c.device.geometry.host_channels=1;SimulationSession s(resolve_configuration(c),&stream);
    s.submit(GlobalRequest{GlobalRequestId{1},Op::Write,GlobalByteAddress{},64,1});auto end=s.advance_before(Tick{2000000});
    check(end.run.status==RunStatus::Paused&&end.run.device.pending_dlus==1,"partial DLU still live");stream.flush();auto snapshot=out.str();
    check(snapshot.find("GlobalFinished")==std::string::npos&&snapshot.find("StreamEnd")==std::string::npos,"flush is neither completion nor closure");
    s.submit(GlobalRequest{GlobalRequestId{2},Op::Write,GlobalByteAddress{64},4032,1,{}, {},s.now()});s.close_input();auto done=s.run_to_completion();
    check(done.run.workload_success,"active contexts span flush");stream.finish(s.now(),"Completed");check(out.str().size()>snapshot.size(),"append after flush");
}
}
int main(int argc,char** argv){
    const std::map<std::string,void(*)()> tests={{"threshold",threshold},{"large_record",large_record},{"finish",finish},{"escaping",escaping},{"write_failure",write_failure},
        {"flush_failure",flush_failure},{"invalid",invalid},{"observational",observational},{"replay",replay},{"partial",partial}};
    try{if(argc!=2)throw std::runtime_error("scenario required");tests.at(argv[1])();std::cout<<"PASS event stream "<<argv[1]<<'\n';}
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
