#include "hbflab/async_module.hpp"
#include "hbflab/capacity.hpp"
#include "ptx_fixture.hpp"
#include <cuda_runtime_api.h>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <cstring>
#include <algorithm>
using namespace hbflab;
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
std::string source(const std::string& mode) {
    auto p=ptx_fixture(R"PTX(
setp.eq.u32 %p0,%r0,0;
@%p0 ret;
mov.u64 %rd5,0;
@%p0 ld.global.u32 %r1,[%rd5];
mov.u32 %r2,0;
and.b32 %r3,%r0,3;
add.u32 %r3,%r3,1;
Loop:
ld.global.u32 %r1,[%rd2];
add.u32 %r2,%r2,1;
setp.lt.u32 %p1,%r2,%r3;
@%p1 bra Loop;
st.global.u32 [%rd3],%r1;
)PTX");
    replace_one(p,"mov.u32 %r0, %tid.x;","mov.u32 %r0, %tid.x; mov.u32 %r4,%ctaid.x; mov.u32 %r5,%ntid.x; mad.lo.u32 %r0,%r4,%r5,%r0;");
    if(mode=="timeout")replace_one(p,"@%p1 bra Loop;","Spin: bra Spin;");
    if(mode=="invalid")replace_one(p,"ld.global.u32 %r1,[%rd2];","ld.global.u32 %r1,[%rd5];");
    if(mode=="wait") {
        replace_one(p,"ld.global.u32 %r1,[%rd2];","mov.u64 %rd6,%globaltimer; ld.global.u32 %r1,[%rd2]; mov.u64 %rd7,%globaltimer; sub.u64 %rd7,%rd7,%rd6;");
        replace_one(p,"st.global.u32 [%rd3],%r1;","st.global.u32 [%rd3],%r1; st.global.u64 [%rd3+8],%rd7;");
    }
    return p;
}
void widths(unsigned* input,unsigned* output,cudaStream_t stream) {
    LiveTimingService service({},16,1);service.register_readonly(input,4096);service.start();
    AsyncModuleInterceptor loader(service);unsigned cases=0;
    unsigned char expected[4096],got[4096];ck(cudaMemcpy(expected,input,4096,cudaMemcpyDeviceToHost));
    for(const auto type:{"u8","s8","b8","u16","s16","b16","u32","s32","b32","u64","s64","b64","f32","f64"})for(unsigned count:{1u,2u,4u}) {
        const unsigned size=std::stoul(std::string(type).substr(1))/8*count;if(size>16)continue;
        std::string regs=std::string(type).substr(1)=="64"?"%rd5":"%r1";
        if(count>1){regs="{";for(unsigned i=0;i<count;++i){if(i)regs+=",";regs+=(std::string(type).substr(1)=="64"?"%rd":"%r")+std::to_string(i+5);}regs+="}";}
        const auto op=std::string(count==1?"":"v"+std::to_string(count)+".")+type;
        const auto p=ptx_fixture("setp.eq.u32 %p0,%r0,999; mov.u64 %rd9,0; @%p0 ld.global."+op+" "+regs+",[%rd9]; @%p0 st.global."+op+" [%rd9],"+regs+"; ld.global."+op+" "+regs+",[%rd2]; st.global."+op+" [%rd3],"+regs+";");
        ck(cudaMemset(output,0,4096));auto m=loader.load_ptx(p);auto f=loader.function(m,"read");loader.launch(f,{1,16},{&input,&output},{output,4096},stream);loader.unload(m);
        ck(cudaMemcpy(got,output,4096,cudaMemcpyDeviceToHost));for(unsigned i=0;i<16;++i)require(std::memcmp(got+16*i,expected+16*i,size)==0,"typed bytes mismatch");++cases;
    }
    service.finish();require(cases==38 && service.statistics().completed==cases*16,"typed coverage/request accounting");
    std::cout<<"typed_cases="<<cases<<" requests="<<cases*16<<'\n';
}
int main(int argc,char** argv)try {
    require(argc==2,"case required");const std::string mode=argv[1];
    unsigned *input=nullptr,*output=nullptr;cudaStream_t a=nullptr,b=nullptr;
    ck(cudaMalloc(reinterpret_cast<void**>(&input),4096));ck(cudaMalloc(reinterpret_cast<void**>(&output),4096));
    unsigned values[1024];for(unsigned i=0;i<1024;++i)values[i]=i+17;
    ck(cudaMemcpy(input,values,sizeof(values),cudaMemcpyHostToDevice));ck(cudaMemset(output,0,4096));
    ck(cudaStreamCreateWithFlags(&a,cudaStreamNonBlocking));ck(cudaStreamCreateWithFlags(&b,cudaStreamNonBlocking));
    if(mode=="widths") {widths(input,output,a);ck(cudaFree(input));ck(cudaFree(output));ck(cudaStreamDestroy(a));ck(cudaStreamDestroy(b));return 0;}
    AsyncRuntimeOptions options;options.audit_limit=20000;options.timeout_ns=1000000000ull;
    options.backend.max_events_per_batch=10000000;
    if(mode=="wait")options.time_scale=10000;
    if(mode=="timeout")options.service_timeout_ns=200000000;
    {
        LiveTimingService service(options,128,2);service.register_readonly(input,4096);
        std::unique_ptr<CapacityStore> capacity;Allocation allocation{};
        if(mode=="capacity") {CapacityOptions o;o.logical_pages=2;o.frames=1;capacity=std::make_unique<CapacityStore>(o,&service);allocation=capacity->allocate(8192);}
        service.start();
        {
            AsyncModuleInterceptor loader(service);const auto p=source(mode);auto m=loader.load_ptx(p);auto f=loader.function(m,"read");
            const auto id=loader.identity(m);require(id.frontend==async_ptx_frontend_version && id.protocol==async_abi_version,"identity versions");
            if(mode=="identity") {
                const auto other=loader.load_ptx(p);const auto oi=loader.identity(other);
                require(id.instance!=oi.instance && id.artifact_sha256==oi.artifact_sha256,"instance/artifact identity");
                require(loader.statistics().cache_hits==1,"cache miss");loader.unload(other);
                rejects([&]{loader.function(other,"read");});
                bool rejected=false;std::thread thread([&]{try{loader.identity(m);}catch(...){rejected=true;}});thread.join();require(rejected,"host ownership");
            }
            rejects([&]{loader.launch(f,{2,32},{&input},{output,4096},a);});
            const bool failure=mode=="invalid" || mode=="store" || mode=="timeout";
            auto* dest=mode=="store"?input:output;
            loader.launch(f,{2,32},{&input,&dest},{dest,4096},a);
            unsigned launches=1;
            if(capacity) {
                unsigned value=0x12345678,got=0;
                capacity->write(allocation,0,&value,sizeof(value));
                capacity->read(allocation,4096,&got,sizeof(got));
                capacity->read(allocation,0,&got,sizeof(got));
                require(got==value,"shared capacity writeback/reload");capacity->flush(allocation);
            }
            if(mode=="concurrent" || mode=="reuse") {
                const unsigned count=mode=="reuse"?10:2;
                for(unsigned i=1;i<count;++i){auto* dst=output+(i%2)*256;loader.launch(f,{2,32},{&input,&dst},{dst,1024},i%2?b:a);++launches;}
            }
            if(failure)rejects([&]{service.synchronize();});
            else {
                loader.unload(m); // Must drain queued launches before unloading executable code.
                rejects([&]{loader.launch(f,{1,1},{&input,&output},{output,4096},a);});
                const auto stats=service.statistics();require(stats.submitted==159*launches && stats.completed==stats.submitted,"predicate/loop request counts");
                require(stats.launches_completed==launches,"launch reclamation");
                if(mode=="concurrent")require(stats.peak_active_launches==2,"no kernel overlap");
                auto audit=service.audit();std::set<std::uint64_t> ids;
                // Runtime appends evidence at completion; independent banks may
                // finish out of order. Dispatch, not vector order, proves admission.
                std::sort(audit.begin(),audit.end(),[](const auto& x,const auto& y){return x.dispatch<y.dispatch;});
                std::map<std::pair<std::uint64_t,unsigned>,std::uint64_t> tickets;
                std::tuple<std::uint64_t,unsigned,std::uint64_t> previous_arrival{};
                for(const auto& t:audit){ids.insert(t.launch);const auto key=std::make_tuple(t.arrival_ns,t.producer,t.ticket);
                    require(key>=previous_arrival,"arrival ordering");previous_arrival=key;
                    auto& ticket=tickets[{t.launch,t.producer}];if(!ticket)ticket=t.ticket;else require(++ticket==t.ticket,"producer ticket ordering");
                    require(t.arrival_ns>=t.grant_floor_ns && t.first_bytes==4,"admission/typed bytes");}
                require(ids.size()==launches,"launch identity reused");
                if(capacity) {
                    auto media=service.take_media_audit();require(!media.empty() && stats.media_completed==media.size(),"shared media completion");
                    std::map<std::uint64_t,std::tuple<std::uint64_t,unsigned,std::uint64_t>> ordered;
                    for(const auto& t:audit)ordered.emplace(t.dispatch,std::make_tuple(t.arrival_ns,t.producer,t.ticket));
                    for(const auto& t:media){require(t.producer==128,"host producer ownership");require(ordered.emplace(t.dispatch,std::make_tuple(t.arrival_ns,t.producer,t.ticket)).second,"duplicate dispatch");}
                    std::tuple<std::uint64_t,unsigned,std::uint64_t> previous{};std::uint64_t dispatch=0;
                    for(const auto& t:ordered){require(t.first==++dispatch && t.second>=previous,"cross-source order");previous=t.second;}
                    require(capacity->statistics().writebacks==1 && stats.backend.writebacks==1,"writeback missing/double charged");
                    capacity->release(allocation);
                }
                unsigned got[1024];ck(cudaMemcpy(got,output,sizeof(got),cudaMemcpyDeviceToHost));
                for(unsigned j=0;j<(launches>1?2u:1u);++j){require(got[j*256]==0,"early return wrote data");for(unsigned i=1;i<64;++i)require(got[j*256+i*4]==values[i*4],"actual GPU bytes mismatch");}
                if(mode=="wait") {
                    std::map<unsigned,AsyncServiceTrace> last_read;for(const auto& t:audit)last_read[t.producer]=t;
                    require(last_read.size()==63,"wait evidence coverage");
                    for(const auto& item:last_read){const auto& t=item.second;std::uint64_t elapsed=0;std::memcpy(&elapsed,got+item.first*4+2,8);
                        require(elapsed>=((t.end_ps-t.arrival_ps)*options.time_scale+999)/1000,"PTX actual load bypassed target delay");}
                }
                std::cout<<"launches="<<launches<<" requests="<<stats.completed<<" peak_launches="<<stats.peak_active_launches<<'\n';
                service.finish();require(service.drained(),"not drained");
            }
        }
    }
    unsigned after[1024];ck(cudaMemcpy(after,input,sizeof(after),cudaMemcpyDeviceToHost));require(std::equal(std::begin(after),std::end(after),std::begin(values)),"readonly bytes changed");
    ck(cudaFree(input));ck(cudaFree(output));ck(cudaStreamDestroy(a));ck(cudaStreamDestroy(b));
    std::cout<<"async module "<<mode<<" passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
