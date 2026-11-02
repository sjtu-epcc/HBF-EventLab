#include "hbflab/kv.hpp"
#include "ptx_fixture.hpp"
#include <iostream>
#include <thread>
#include <random>
using namespace hbflab;
CapacityOptions options(){CapacityOptions o;o.logical_pages=8;o.frames=2;o.backend.geometry={1,1,1,4,2,1};return o;}
int main(int argc,char** argv) try {
    const std::string test=argc>1?argv[1]:"paging";auto o=options();
    if(test=="failure" || test=="kv_failure"){o.backend.fail_program_after=8;o.frames=1;}
    CapacityStore store(o);
    if(test=="writeback" || test=="striped_replay") {
        if(test=="striped_replay")o.backend.geometry={1,1,2,4,2,1};
        const unsigned pages=test=="striped_replay"?6:8;
        TimingBackend backend(o.backend);backend.reserve(pages*4096);
        require(backend.read(4096,64).status==Status::Ok,"warm old page");
        std::array<std::uint64_t,64> tokens{};for(unsigned i=0;i<64;++i)tokens[i]=900+i;
        require(backend.write_page(4096,tokens).status==Status::Ok,"nonzero page writeback");
        for(unsigned page=0;page<pages;++page)require(backend.read(page*4096,4096).status==Status::Ok,"block preservation/token coherence");
        require(backend.statistics().replayed_pages==(test=="striped_replay"?3:4) && backend.statistics().device.erases>0,"real block replay/erase");
    } else if(test=="paging" || test=="flush") {
        const auto a=store.allocate(6*4096-7);std::vector<unsigned char> input(store.size(a)),out(input.size());
        for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<unsigned char>(i*23+i/4096);
        store.write(a,0,input.data(),input.size());if(test=="flush")store.flush_all();store.read(a,0,out.data(),out.size());require(input==out,"oversubscribed bytes");
        const auto stats=store.statistics();require(stats.resident_frames==2 && stats.evictions>0 && stats.writebacks>=4,"finite frame/writeback counts");
        store.flush(a);require(!store.statistics().dirty_frames,"flush cleaned frames");store.release(a);require(store.free_pages()==8,"capacity reclaimed");
    } else if(test=="leases") {
        const auto a=store.allocate(4096);const auto first=store.pin(a,0),second=store.pin(a,0);
        require(store.info(first).frame==store.info(second).frame,"shared readers use same frame");store.unpin(first);
        rejects([&]{store.pin(a,0,PageAccess::Write);});store.unpin(second);
        const auto writer=store.pin(a,0,PageAccess::Write);rejects([&]{store.info(first);});
        CapacityStore other(o);rejects([&]{other.info(writer);});store.unpin(writer);
    } else if(test=="lru") {
        const auto a=store.allocate(3*4096);auto l=store.pin(a,0);const auto zero=store.info(l).frame;store.unpin(l);
        l=store.pin(a,1);const auto one=store.info(l).frame;store.unpin(l);l=store.pin(a,0);store.unpin(l);
        l=store.pin(a,2);require(store.info(l).frame==one,"deterministic LRU victim");store.unpin(l);
        l=store.pin(a,0);require(store.info(l).frame==zero && store.statistics().faults==3,"recent frame retained");store.unpin(l);
    } else if(test=="random") {
        const auto a=store.allocate(6*4096);std::vector<unsigned char> reference(store.size(a),0);std::mt19937 random(12345);
        for(unsigned step=0;step<120;++step) {
            const auto offset=random()%reference.size();const auto count=(std::min)(std::size_t{1+random()%5000},reference.size()-offset);
            std::vector<unsigned char> bytes(count);
            if(step%3) {for(auto& b:bytes)b=static_cast<unsigned char>(random());store.write(a,offset,bytes.data(),count);std::copy(bytes.begin(),bytes.end(),reference.begin()+offset);}
            else {store.read(a,offset,bytes.data(),count);require(std::equal(bytes.begin(),bytes.end(),reference.begin()+offset),"random paging oracle");}
            if(step%19==0)store.flush_all();
        }
        std::vector<unsigned char> actual(reference.size());store.read(a,0,actual.data(),actual.size());require(actual==reference,"final random byte oracle");
    } else if(test=="pins") {
        const auto a=store.allocate(3*4096);const auto x=store.pin(a,0),y=store.pin(a,1);
        rejects([&]{store.pin(a,2);});rejects([&]{store.release(a);});rejects([&]{store.flush(a);});
        rejects([&]{store.pin(a,0,PageAccess::Write);});unsigned v=3;rejects([&]{store.write_pinned(x,0,&v,4);});
        store.unpin(x);rejects([&]{store.unpin(x);});const auto z=store.pin(a,2);
        require(store.info(z).frame!=store.info(y).frame,"pinned frame never evicted");store.unpin(y);store.unpin(z);
        const auto writer=store.pin(a,0,PageAccess::Write);rejects([&]{store.pin(a,0);});store.write_pinned(writer,1,&v,4);store.unpin(writer);
        store.release(a);rejects([&]{store.size(a);});
    } else if(test=="reuse") {
        const auto a=store.allocate(4096);unsigned x=0xdeadbeef;store.write(a,0,&x,4);store.flush(a);store.release(a);
        const auto b=store.allocate(4096);require(a.value!=b.value,"no allocation ABA");unsigned y=1;store.read(b,0,&y,4);require(y==0,"reuse zero initialization");store.flush(b);store.release(b);
        const auto c=store.allocate(4096);const auto before=store.statistics().writebacks;store.flush_all();
        require(store.statistics().writebacks==before+1,"flush materializes untouched recycled zeros");store.read(c,0,&y,4);require(y==0,"recycled flush bytes");
    } else if(test=="failure") {
        const auto a=store.allocate(4096);unsigned x=42;store.write(a,0,&x,4);rejects([&]{store.flush(a);});
        require(store.statistics().poisoned && store.statistics().dirty_frames==1 && store.statistics().writebacks==0,"failed write retains dirty authority");
        rejects([&]{store.read(a,0,&x,4);});
    } else if(test=="ownership") {
        const auto a=store.allocate(4096);CapacityStore other(o);rejects([&]{other.size(a);});
        bool rejected=false;std::thread t([&]{try{store.size(a);}catch(...){rejected=true;}});t.join();require(rejected,"host owner enforced");
    } else if(test=="limits") {
        rejects([&]{store.allocate(0);});auto a=store.allocate(8*4096);rejects([&]{store.allocate(1);});
        unsigned x=0;rejects([&]{store.write(a,8*4096-1,&x,4);});require(store.statistics().faults==0,"prevalidation before I/O");
        auto bad=o;bad.frames=0;rejects([&]{CapacityStore invalid(bad);});
    } else if(test=="kv_failure") {
        KvOptions config;config.head_dimension=4;config.element_bytes=4;config.tokens_per_block=1;
        KvCache kv(store,config);auto sequence=kv.create();std::array<unsigned,4> row{1,2,3,4};
        rejects([&]{kv.append(sequence,row.data(),row.data(),sizeof(row));});
        require(kv.poisoned() && store.statistics().dirty_frames==1 && store.statistics().writebacks==0,"failed partial KV append is terminal, dirty retained");
        rejects([&]{kv.locate(sequence,0,0,KvKind::Key);});
    } else if(test=="kv_cross_page") {
        KvOptions config;config.head_dimension=6000;config.element_bytes=1;config.tokens_per_block=1;
        {
            KvCache kv(store,config);auto seq=kv.create();std::vector<unsigned char> k(6000),v(6000),out(6000);
            for(unsigned i=0;i<6000;++i){k[i]=static_cast<unsigned char>(i);v[i]=static_cast<unsigned char>(i+19);}
            for(unsigned token=0;token<2;++token)kv.append(seq,k.data(),v.data(),k.size());
            kv.read(seq,0,0,KvKind::Key,out.data(),out.size());require(k==out,"cross-page K");
            kv.read(seq,0,1,KvKind::Value,out.data(),out.size());require(v==out,"cross-page V");
        }
        require(store.free_pages()==8,"KV destructor reclaims unpinned allocations");
    } else if(test.find("kv_")==0) {
        KvOptions config;config.layers=2;config.heads=1;config.head_dimension=16;config.element_bytes=4;config.tokens_per_block=2;config.max_tokens=6;
        KvCache kv(store,config);const auto seq=kv.create();std::array<unsigned,32> key{},value{};
        for(unsigned token=0;token<4;++token){key.fill(token+10);value.fill(token+100);kv.append(seq,key.data(),value.data(),sizeof(key));}
        require(kv.length(seq)==4,"KV publication");std::array<unsigned,16> row{};
        for(unsigned token=0;token<4;++token)for(unsigned layer=0;layer<2;++layer) {
            kv.read(seq,layer,token,KvKind::Key,row.data(),sizeof(row));for(auto v:row)require(v==token+10,"K bytes");
            kv.read(seq,layer,token,KvKind::Value,row.data(),sizeof(row));for(auto v:row)require(v==token+100,"V bytes");
        }
        if(test=="kv_capacity") {rejects([&]{kv.append(seq,key.data(),value.data(),sizeof(key));});require(kv.length(seq)==4 && !kv.poisoned(),"capacity rejection before publication");}
        if(test=="kv_truncate") {
            kv.truncate(seq,1);require(store.free_pages()==4,"full KV block reclaimed");key.fill(99);value.fill(199);kv.append(seq,key.data(),value.data(),sizeof(key));
            kv.read(seq,1,1,KvKind::Value,row.data(),sizeof(row));for(auto v:row)require(v==199,"tail overwritten after truncate");rejects([&]{kv.locate(seq,0,2,KvKind::Key);});
        }
        auto slice=kv.locate(seq,0,0,KvKind::Key);auto lease=store.pin(slice.allocation,0);rejects([&]{kv.truncate(seq,0);});store.unpin(lease);
        kv.flush(seq);kv.release(seq);require(store.free_pages()==8,"KV teardown reclaimed pages");rejects([&]{kv.length(seq);});
    } else throw std::invalid_argument("unknown case");
    std::cout<<test<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
