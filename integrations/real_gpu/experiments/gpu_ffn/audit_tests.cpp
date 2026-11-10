#include "online_audit.hpp"
#include <functional>
#include <iostream>
using namespace hbf_eventlab;
static void check(bool ok){if(!ok)throw std::runtime_error("test failed");}
static void rejected(std::function<void()> f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught);}
static void define(OnlineAudit& a,std::uint64_t global,std::uint64_t base,std::uint64_t first) {
    a.observe(StatGlobalDefined{GlobalRequestId{global},Op::Read,4096,Tick{0}});
    for(std::uint64_t i=0;i<64;++i)a.observe(StatWorkDefined{LogicalWorkId{first+i},GlobalRequestId{global},{0,LocalByteAddress{base+i*64}},std::nullopt});
}
static void finish(OnlineAudit& a,std::uint64_t global,std::uint64_t first) {
    for(std::uint64_t i=0;i<64;++i)a.observe(StatWorkFinished{LogicalWorkId{first+i},Tick{123},true,{}});
    a.observe(StatGlobalFinished{GlobalRequestId{global},Tick{123},true,{}});
}
int main(){try{
    // Completed history does not grow the observer's live maps.
    OnlineAudit a;constexpr std::uint64_t n=10000;a.begin(Op::Read,0,n*4096);
    for(std::uint64_t g=0;g<n;++g){define(a,g+1,g*4096,g*64+1);finish(a,g+1,g*64+1);}
    a.end(n,n,123);check(a.total_read_fragments==n*64&&a.peak_work==64);
    rejected([]{OnlineAudit x;x.begin(Op::Read,4096,4096);define(x,1,0,1);});
    rejected([]{OnlineAudit x;x.begin(Op::Read,0,4096);define(x,1,0,1);finish(x,1,1);x.observe(StatWorkFinished{LogicalWorkId{1},Tick{123},true,{}});});
    rejected([]{OnlineAudit x;x.begin(Op::Read,0,4096);define(x,1,0,1);x.end(1,1,123);});
    rejected([]{OnlineAudit x;x.begin(Op::Read,0,4096);define(x,1,0,1);finish(x,1,1);x.end(1,1,124);});
    rejected([]{OnlineAudit x;x.begin(Op::Read,0,4096);define(x,1,0,1);x.observe(StatWorkFinished{LogicalWorkId{1},Tick{123},false,"injected"});for(std::uint64_t i=2;i<=64;++i)x.observe(StatWorkFinished{LogicalWorkId{i},Tick{123},true,{}});x.observe(StatGlobalFinished{GlobalRequestId{1},Tick{123},false,"injected"});x.end(1,1,123);});
    std::cout<<"Online audit bounded-history and corruption tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
