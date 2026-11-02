#include "hbflab/backend.hpp"
#include <iostream>
#include <map>
#include <string>
using namespace hbflab;
void test(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F>void reject(F f){bool caught=false;try{f();}catch(...){caught=true;}test(caught,"expected rejection");}
int main(int argc,char** argv)try {
    test(argc==2,"case required");const std::string mode=argv[1];BackendOptions o;
    if(mode=="pressure"){o.outstanding=1;o.resident_fragments=64;o.timing.buffers.ingress={1,4096};o.timing.buffers.egress={1,64};}
    if(mode=="failure")o.fail_program_after=4;
    if(mode=="overlap"){o.timing.media_timing.program=hbf_eventlab::Tick{10000000};}
    TimingBackend b(o);b.reserve(4*4096);b.begin_stream(16);const auto at=b.now_ps()+10;
    std::array<std::uint64_t,64> a{},c{};for(unsigned i=0;i<64;++i){a[i]=10000+i;c[i]=20000+i;}
    if(mode=="validation") {
        reject([&]{b.submit_write(0,at,0,a);});reject([&]{b.submit_write(1,at,1,a);});
        reject([&]{b.submit_write(1,at,16384,a);});reject([&]{b.submit_write(1,at-10,0,a);});
        b.submit_write(3,at,0,a);reject([&]{b.submit_read(3,{at,{{0,64}}});});
        reject([&]{b.submit_write(4,at-1,0,a);});reject([&]{b.end_stream();});
    }else if(mode=="watermark") {
        b.submit_write(1,at,0,a);test(!b.advance_before(at),"exclusive write watermark");
        test(b.advance_before(at+1),"write arrival below watermark");
        while(b.advance_before(at+1)){}test(b.take_completed_writes().empty(),"early write completion");
    }else if(mode=="overlap") {
        b.submit_write(1,at,0,a);b.submit_read(2,{at,{{4096,64}}});b.submit_read(3,{at,{{8192,64}}});
    }else if(mode=="read_write_read") {
        b.submit_read(9,{at,{{0,4096}}});b.submit_write(2,at,0,a);b.submit_read(1,{at,{{0,4096}}});
    }else if(mode=="failure")b.submit_write(1,at,0,a);
    else if(mode=="replay" || mode=="pressure") {
        b.submit_write(8,at,0,a);b.submit_write(2,at,8192,c);
        b.submit_read(1,{at,{{0,4096}}});b.submit_read(7,{at,{{8192,4096}}});
        b.submit_read(3,{at,{{4096,4096}}});b.submit_read(4,{at,{{12288,4096}}});
    }else throw std::runtime_error("unknown case");
    if(mode=="failure") {
        reject([&]{while(b.advance_before(UINT64_MAX)){};b.take_completed_writes();});
        test(!b.drained(),"failure did not poison device");reject([&]{b.submit_read(4,{b.now_ps()+1,{{0,4}}});});
    }else {
        std::map<std::uint64_t,ReadResult> reads;std::map<std::uint64_t,WriteResult> writes;
        do {
            for(auto r:b.take_completed())reads.emplace(r.id,r.result);
            for(auto w:b.take_completed_writes())writes.emplace(w.id,w.result);
        }while(b.advance_before(UINT64_MAX));
        for(auto r:b.take_completed())reads.emplace(r.id,r.result);
        for(auto w:b.take_completed_writes())writes.emplace(w.id,w.result);
        b.end_stream();test(b.drained(),"shared device did not drain");
        for(const auto& r:reads)test(r.second.status==Status::Ok,"symbolic read content mismatch");
        for(const auto& w:writes)test(w.second.status==Status::Ok && w.second.replayed_pages==2,"block replay mismatch");
        if(mode=="read_write_read")test(reads.at(9).end_ps<writes.at(2).end_ps && writes.at(2).end_ps<reads.at(1).end_ps,"RAW/WAR submission order ignored IDs");
        if(mode=="overlap")test(reads.at(2).end_ps<writes.at(1).end_ps && reads.at(3).end_ps>writes.at(1).end_ps,"different block overlap / same block exclusion");
        if(mode=="replay" || mode=="pressure") {
            test(writes.at(8).end_ps<writes.at(2).end_ps,"write order regression");
            test(b.statistics().writebacks==2 && b.statistics().replayed_pages==4 && b.statistics().dependency_waits>=3,"write/dependency counters");
            test(b.read(0,4096).status==Status::Ok && b.read(8192,4096).status==Status::Ok,"preserved tokens after replay");
        }
        test(b.statistics().peak_resident_fragments<=o.resident_fragments,"shared Host resident budget exceeded");
    }
    std::cout<<"shared backend "<<mode<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
