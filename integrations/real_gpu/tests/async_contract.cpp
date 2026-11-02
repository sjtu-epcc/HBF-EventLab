#include "hbflab/async_order.hpp"
#include "hbflab/backend.hpp"
#include <iostream>
#include <random>
#include <string>
using namespace hbflab;
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"expected rejection");}
int main(int argc,char** argv){try{
    const std::string name=argc>1?argv[1]:"";
    if(name=="ordering") {
        AsyncOrder q(4,100,1000,1);
        q.grant(0,1,1000);q.grant(1,1,1000);q.grant(2,1,1000);
        q.publish(2,1,110);q.publish(0,1,120);
        check(q.ready().empty(),"unknown producer must hold later arrivals");
        q.publish(1,1,110);const auto sorted=q.ready();
        check(sorted.size()==3 && sorted[0].producer==1 && sorted[1].producer==2 && sorted[2].producer==0,"arrival/owner deterministic order");
        check(q.unknown_floor()==UINT64_MAX,"parked producer cannot block resident work");
        // A newly started CTA cannot inject an earlier timestamp ahead of a
        // future arrival which has already been committed to the event queue.
        const auto floor=q.grant(3,1,1000);
        check(q.map(floor)>sorted.back().arrival_ps,"new credit must follow sealed horizon");
    }else if(name=="identity") {
        AsyncOrder q(2,100,0,1);
        rejects([&]{q.publish(0,1,200);});rejects([&]{q.grant(0,2,0);});
        const auto floor=q.grant(0,1,0);
        rejects([&]{q.grant(0,1,0);});rejects([&]{q.publish(1,1,floor);});
        rejects([&]{q.publish(0,2,floor);});rejects([&]{q.publish(0,1,floor-1);});
        q.publish(0,1,floor);rejects([&]{q.publish(0,1,floor);});
        rejects([&]{q.complete(0,1);});q.ready();rejects([&]{q.complete(0,2);});
        q.complete(0,1);rejects([&]{q.grant(0,1,1000);});
        q.grant(0,2,1000);rejects([&]{q.grant(2,1,1000);});
    }else if(name=="scaling") {
        for(auto scale:{1u,3u,1000u,1000000u}) {
            AsyncOrder q(2,90,777,scale);
            const auto f=q.grant(0,1,778);
            check(q.map(f)>778,"ceil floor must map strictly after now");
            q.grant(1,1,778);q.publish(0,1,f);
            check(q.ready().empty(),"rounded equal ticks require sealing");
            q.publish(1,1,f);check(q.ready().size()==2,"equal ticks ready together");
        }
        rejects([]{AsyncOrder q(0,0,0,1);});rejects([]{AsyncOrder q(8193,0,0,1);});
        AsyncOrder q(1,UINT64_MAX-1,0,1);rejects([&]{q.grant(0,1,100000);});
        AsyncOrder r(1,0,UINT64_MAX,1);rejects([&]{r.grant(0,1,UINT64_MAX);});
        AsyncOrder sentinel(1,0,UINT64_MAX-1000,1);rejects([&]{sentinel.grant(0,1,UINT64_MAX-1);});
    }else if(name=="random") {
        AsyncOrder q(127,100,1000,3);std::mt19937 rng(42);std::uint64_t now=1000,last=0;
        for(unsigned n=0;n<10000;++n) {
            const auto i=rng()%127;const auto s=q.slot(i);
            if(s.state==AsyncOrder::State::Parked)q.grant(i,s.ticket+1,now);
            else if(s.state==AsyncOrder::State::Granted)q.publish(i,s.ticket,s.floor_ns+rng()%300);
            for(const auto& r:q.ready()) {
                check(r.arrival_ps>=last,"random arrival regression");last=r.arrival_ps;
                q.complete(r.producer,r.ticket);
            }
            const auto bound=q.unknown_floor();if(bound!=UINT64_MAX)now=std::max(now,bound-1);
        }
    }else if(name=="equivalence") {
        TimingBackend batch,stream;batch.reserve(8192);stream.reserve(8192);
        const auto at=batch.now_ps();
        const std::vector<TimedRead> input{{at+10,{{0,256},{32,32}}},{at+10,{{4090,17}}},{at+100,{{4096,128}}},{at+100000000,{{128,4}}}};
        const auto reference=batch.read_batch(input);stream.begin_stream(input.size());
        for(unsigned i=0;i<input.size();++i)stream.submit_read(i+1,input[i]);
        std::vector<ReadResult> actual(input.size());
        do {for(const auto& r:stream.take_completed())actual.at(r.id-1)=r.result;}while(stream.advance_before(UINT64_MAX));
        for(const auto& r:stream.take_completed())actual.at(r.id-1)=r.result;
        stream.end_stream();
        for(unsigned i=0;i<actual.size();++i)check(actual[i].status==reference[i].status && actual[i].begin_ps==reference[i].begin_ps &&
            actual[i].end_ps==reference[i].end_ps && actual[i].sectors==reference[i].sectors,"incremental engine must preserve original backend timing");
    }else {
        BackendOptions o;
        if(name=="pressure"){o.outstanding=1;o.timing.buffers.ingress={1,4096};o.timing.buffers.egress={1,64};o.resident_fragments=64;}
        if(name=="budget")o.max_events_per_batch=100;
        TimingBackend b(o);b.reserve(8192);const auto start=b.now_ps();b.begin_stream(8);
        if(name=="validation") {
            rejects([&]{b.reserve(4096);});rejects([&]{b.read(0,4);});
            rejects([&]{b.write_page(0,{});});
            rejects([&]{b.submit_read(1,{start,{{0,4}}});});
            rejects([&]{b.submit_read(0,{start+1,{{0,4}}});});
            rejects([&]{b.submit_read(1,{UINT64_MAX,{{0,4}}});});
            rejects([&]{b.submit_read(1,{start+1,{{8192,4}}});});
            rejects([&]{b.submit_read(1,{start+1,{{0,4097}}});});
            b.submit_read(1,{start+10,{{0,4}}});
            rejects([&]{b.submit_read(1,{start+10,{{0,4}}});});
            rejects([&]{b.submit_read(2,{start+9,{{0,4}}});});
            rejects([&]{b.end_stream();});
        }else if(name=="watermark") {
            b.submit_read(1,{start+10,{{0,4}}});
            check(!b.advance_before(start+10) && b.now_ps()==start,"exclusive arrival bound");
            check(b.advance_before(start+11) && b.now_ps()==start+10,"arrival admitted below bound");
            while(b.advance_before(start+11)){}
            check(b.take_completed().empty(),"response must remain blocked by watermark");
        }else if(name=="individual") {
            b.submit_read(1,{start+10,{{0,4}}});b.submit_read(2,{start+100000000,{{4096,4}}});
            while(b.advance_before(start+100000000)){}
            const auto result=b.take_completed();
            check(result.size()==1 && result[0].id==1 && result[0].result.end_ps<start+100000000,"independent completion before future arrival");
            check(!b.drained(),"future request retained");
        }else if(name=="pressure" || name=="budget" || name=="parent_bound") {
            for(unsigned i=0;i<8;++i)b.submit_read(i+1,{start+10,{{name=="pressure"?0:i*512u,name=="pressure"?4096u:512u}}});
            if(name=="parent_bound") {
                rejects([&]{b.submit_read(9,{start+10,{{0,4}}});});
                while(b.advance_before(UINT64_MAX)){}
                rejects([&]{b.submit_read(9,{b.now_ps()+1,{{0,4}}});});
                check(!b.drained(),"unconsumed results count toward bound");
            }
        }else throw std::runtime_error("unknown test");
        if(name=="budget") {
            rejects([&]{while(b.advance_before(UINT64_MAX)){};});
            check(!b.drained(),"event budget poisons streaming backend");
            rejects([&]{b.take_completed();});
        }else {
            while(b.advance_before(UINT64_MAX)){}
            const auto result=b.take_completed();for(const auto& r:result)check(r.result.status==Status::Ok,"stream success");
            b.end_stream();check(b.drained(),"stream drains");
            if(name=="pressure")check(b.statistics().backpressure>0 && b.statistics().peak_resident_fragments<=o.resident_fragments && result.size()==8,"bounded Host input backpressure");
            check(b.read(0,4).status==Status::Ok,"synchronous reuse after stream end");
        }
    }
    std::cout<<"PASS "<<name<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
