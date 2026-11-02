#include "hbflab/kv.hpp"
#include "ptx_fixture.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <chrono>
using namespace hbflab;
CapacityOptions config(){CapacityOptions o;o.logical_pages=8;o.frames=2;o.backend.geometry={1,1,1,2,4,1};return o;}
struct TempFile {
    std::filesystem::path path;
    TempFile(const std::string& test):path(std::filesystem::temp_directory_path()/("hbflab-"+test+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pages")){}
    ~TempFile(){std::error_code ec;std::filesystem::remove(path,ec);}
};
// Allows deterministic idle advances without wall-clock sleeps. All media I/O
// still uses the actual EventLab backend, including block replay and failures.
struct Media:CapacityMedia {
    TimingBackend backend;
    hbf_eventlab::Geometry geometry;
    explicit Media(BackendOptions o):backend(o),geometry(o.geometry){}
    std::uint64_t reserve_media(std::uint64_t n) override{return backend.reserve(n);}
    ReadResult read_media(std::uint64_t a,std::uint32_t n) override{return backend.read(a,n);}
    WriteResult write_media(std::uint64_t a,const std::array<std::uint64_t,64>& t) override{return backend.write_page(a,t);}
    BackendStats media_statistics() override{return backend.statistics();}
    std::uint64_t media_time_ps() override{return backend.now_ps();}
    void abort_media() override{}
    std::optional<hbf_eventlab::Geometry> media_geometry() override{return geometry;}
    void idle(std::uint64_t n){backend.read_batch({{backend.now_ps()+n,{}}});}
};
int main(int argc,char** argv)try {
    const std::string test=argc>1?argv[1]:"file_reopen";TempFile temp(test);auto o=config();
    if(test.rfind("file_",0)==0 || test=="kv_file") {
        o.backing_mode=BackingMode::CreateFile;o.backing_path=temp.path;
        if(test=="file_failure")o.backend.fail_program_after=8;
        if(test=="file_unclean") {
            {CapacityStore s(o);auto a=s.allocate(4096);s.flush_all();unsigned v=9;s.write(a,0,&v,4);}
            o.backing_mode=BackingMode::OpenFile;rejects([&]{CapacityStore s(o);});
        } else if(test=="file_exclusive") {
            CapacityStore s(o);s.flush_all();rejects([&]{CapacityStore other(o);});o.backing_mode=BackingMode::OpenFile;rejects([&]{CapacityStore other(o);});
        } else if(test=="file_failure") {
            {CapacityStore s(o);auto a=s.allocate(4096);unsigned v=9;s.write(a,0,&v,4);rejects([&]{s.flush_all();});
             require(s.statistics().poisoned && !s.statistics().file_write_bytes && s.statistics().dirty_frames==1,"failed media never commits file bytes");}
            o.backing_mode=BackingMode::OpenFile;rejects([&]{CapacityStore s(o);});
        } else if(test=="file_corrupt") {
            {CapacityStore s(o);s.flush_all();}
            std::filesystem::resize_file(temp.path,70);o.backing_mode=BackingMode::OpenFile;rejects([&]{CapacityStore s(o);});
        } else if(test=="kv_file") {
            CapacityStore s(o);KvOptions k;k.layers=2;k.heads=1;k.head_dimension=16;k.element_bytes=4;k.tokens_per_block=1;k.max_tokens=2;
            KvCache kv(s,k);const auto seq=kv.create();std::array<unsigned,32> key{},value{};key.fill(19);value.fill(37);
            kv.append(seq,key.data(),value.data(),sizeof(key));kv.append(seq,value.data(),key.data(),sizeof(key));
            std::array<unsigned,16> out{};kv.read(seq,1,1,KvKind::Value,out.data(),sizeof(out));for(auto v:out)require(v==19,"KV file roundtrip");
            s.flush_all();require(s.statistics().file_write_bytes && s.statistics().file_read_bytes,"KV really used file I/O");kv.release(seq);s.flush_all();
        } else {
            std::vector<unsigned char> expected(5*4096-7);for(std::size_t i=0;i<expected.size();++i)expected[i]=static_cast<unsigned char>(i*31+i/4096);
            Allocation old{};
            {CapacityStore s(o);old=s.allocate(expected.size());s.write(old,0,expected.data(),expected.size());s.flush_all();
             require(s.statistics().checkpoints==1 && s.statistics().file_write_bytes==5*4096,"file checkpoint bytes");
             if(test=="file_reuse"){s.release(old);auto a=s.allocate(expected.size());std::fill(expected.begin(),expected.end(),0);s.flush_all();require(s.size(a)==expected.size(),"reuse allocation");}}
            o.backing_mode=BackingMode::OpenFile;
            {CapacityStore s(o);const auto restored=s.allocations();require(restored.size()==1,"allocation catalog restored");rejects([&]{s.size(old);});
             require(restored[0].store!=old.store,"reopened handles have fresh store identity");std::vector<unsigned char> out(expected.size());s.read(restored[0],0,out.data(),out.size());require(out==expected,"persistent byte oracle including partial tail");
             require(s.statistics().resident_frames==2 && s.statistics().file_read_bytes==5*4096,"bounded reopened frames");
             s.release(restored[0]);s.flush_all();}
            {CapacityStore s(o);require(s.allocations().empty(),"released catalog persisted");}
        }
    } else if(test=="prefetch" || test=="prefetch_bounds") {
        o.frames=3;o.read_ahead_pages=test=="prefetch"?1:0;CapacityStore s(o);const auto a=s.allocate(4*4096);
        if(test=="prefetch_bounds") {
            rejects([&]{s.prefetch(a,4,1);});rejects([&]{s.prefetch(a,0,5);});
            const auto x=s.pin(a,0,PageAccess::Write),y=s.pin(a,1),z=s.pin(a,2);
            unsigned v=81;s.write_pinned(x,0,&v,4);const auto before=s.statistics();require(s.prefetch(a,3,1)==0,"no prefetch under frame pressure");
            require(s.statistics().writebacks==before.writebacks && s.statistics().evictions==before.evictions,"no dirty/pinned/demand eviction");s.unpin(z);s.unpin(y);s.unpin(x);
        } else {
            auto l=s.pin(a,0);s.unpin(l);auto before=s.statistics();require(before.prefetch_fills==1 && before.faults==2,"automatic next-page fill");
            l=s.pin(a,1);s.unpin(l);require(s.statistics().prefetch_useful==1,"prefetch consumed by demand");
            l=s.pin(a,2);s.unpin(l);require(s.statistics().prefetch_useful==2,"next-page reuse");
            const auto requests=s.statistics().backend.requests;l=s.pin(a,1);s.unpin(l);require(s.statistics().backend.requests==requests,"cache hit has no duplicate media charge");
        }
    } else if(test=="thermal" || test=="retention") {
        EnvironmentOptions e;e.enabled=true;e.ambient_c=e.initial_c=e.reference_c=25;e.idle_w=0;e.gpu_coupling=1;e.resistance_k_per_w=1;e.capacitance_j_per_k=1;e.retention_seconds=10;
        EnvironmentModel m(e);BackendStats b;m.sample(0,b,0);
        if(test=="thermal") {
            m.sample(1000000000000ull,b,10);const auto expected=25+10*(1-std::exp(-1.0));require(std::abs(m.statistics().temperature_c-expected)<1e-10,"analytic RC heating");
            m.sample(2000000000000ull,b,0);require(std::abs(m.statistics().temperature_c-(25+(expected-25)*std::exp(-1.0)))<1e-10,"analytic RC cooling");
            rejects([&]{m.sample(1,b,0);});rejects([&]{m.sample(2000000000000ull,b,-1);});e.retention_seconds=0;rejects([&]{EnvironmentModel invalid(e);});
        } else {
            m.sample(1000000000000ull,b,0);require(std::abs(m.statistics().exposure-0.1)<1e-12,"reference retention exposure");
            e.ambient_c=e.initial_c=35;EnvironmentModel hot(e);hot.sample(0,b,0);hot.sample(1000000000000ull,b,0);
            require(std::abs(hot.statistics().exposure-0.2)<1e-12,"10 degree acceleration doubles exposure");
            b.sectors=1;m.sample(2000000000000ull,b,0);require(m.statistics().media_energy_j==64*e.read_j_per_byte,"byte energy accounted");
            b.sectors=0;rejects([&]{m.sample(3000000000000ull,b,0);});
        }
    } else if(test.rfind("refresh",0)==0) {
        o.environment.enabled=true;o.environment.ambient_c=o.environment.initial_c=o.environment.reference_c=25;
        o.environment.retention_seconds=1;o.environment.idle_w=0;o.environment.read_j_per_byte=0;o.environment.program_j_per_byte=0;
        if(test=="refresh_failure")o.backend.fail_program_after=8;
        Media media(o.backend);CapacityStore s(o,&media);auto a=s.allocate(4*4096);
        unsigned v=99;auto lease=s.pin(a,0,PageAccess::Write);s.write_pinned(lease,0,&v,4);s.unpin(lease);
        if(test=="refresh_failure") {
            media.idle(900000000000ull);rejects([&]{s.maintain();});require(s.statistics().poisoned && !s.statistics().refreshes && s.statistics().dirty_frames==1,"refresh failure retains dirty authority");
        } else if(test=="refresh_pinned") {
            const auto b=s.pin(a,2),c=s.pin(a,3);media.idle(1100000000000ull);s.maintain();
            require(s.statistics().refresh_blocked==2 && s.statistics().retention_expirations>=2,"leased refresh deferred and deadline miss visible");s.unpin(b);s.unpin(c);
            require(s.maintain()==1,"deferred refresh progresses after release");
        } else {
            media.idle(900000000000ull);const auto before=s.statistics();require(s.maintain()==1,"bounded refresh work");auto after=s.statistics();
            require(after.refreshes==1 && after.refresh_pending==2,"block replay resets age of both pages, budget leaves other block pending");
            require(after.backend.requests==before.backend.requests+1 && after.backend.replayed_pages==before.backend.replayed_pages+2,"refresh uses real READ and full block replay");
            require(s.maintain()==1 && s.statistics().refresh_pending==0 && s.statistics().retention_expirations==0,"second block drained before deadline");unsigned got=0;s.read(a,0,&got,4);require(got==99,"refresh preserves dirty latest bytes");
        }
    } else throw std::invalid_argument("unknown test");
    std::cout<<test<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
