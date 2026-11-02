#include "hbflab/ptx.hpp"
#include "ptx_fixture.hpp"
#include <iostream>
using namespace hbflab;
int main(int argc,char** argv)try {
    const std::string mode=argc>1?argv[1]:"control";
    if(mode=="control") {
        auto source=ptx_fixture("L: @%p0 ret; ld.global.u32 %r1,[%rd2]; @!%p0 bra L;");
        const auto r=rewrite_async_ptx(source);
        require(r.text.find("__hbflab_view[64]")!=std::string::npos,"hidden view absent");
        require(r.text.find("call.uni")==std::string::npos,"collective call in divergent code");
        require(r.text.find("@%p0 ret;")!=std::string::npos,"early return lost");
        require(r.text.find("[__hbflab_op], 2")!=std::string::npos,"loop health check absent");
        require(r.entries[0].parameter_bytes.size()==2 && r.entries[0].loads==1,"public manifest changed");
        rejects([&]{rewrite_ptx(source);});
    }else if(mode=="labels") {
        for(const auto body:{"bra missing;", "L: L:", "@%p0 bra %r1;", "bra 1;", "L: bra.uni L; L:"})
            rejects([&]{rewrite_async_ptx(ptx_fixture(std::string(body)+"ld.global.u32 %r1,[%rd2];"));});
        require(rewrite_async_ptx(ptx_fixture("bra $L; $L: ld.global.u32 %r1,[%rd2];")).entries.size()==1,"forward label");
    }else if(mode=="predicate") {
        const auto r=rewrite_async_ptx(ptx_fixture("@!%p0 ld.global.v4.u32 {%r1,%r2,%r3,%r4},[%rd2];"));
        require(r.text.find("@!%p0 mov.u32 %__hbflab_active, 1;")!=std::string::npos,"predicate lost");
        require(r.text.find("@!%__hbflab_p ret;")!=std::string::npos,"failure must stop execution");
        require(r.entries[0].accesses[0].bytes==16 && r.entries[0].accesses[0].predicate=="@!%p0","access manifest");
    }else if(mode=="reject") {
        for(const auto body:{"call foo;", "bar.sync 0;", "ld.shared.u32 %r1,[%rd2];", "atom.global.add.u32 %r1,[%rd2],1;", "cp.async.ca.shared.global;", "exit;"})
            rejects([&]{rewrite_async_ptx(ptx_fixture(body));});
        rejects([&]{rewrite_async_ptx(ptx_fixture()+".global .u64 __hbflab_output;");});
    }else throw std::runtime_error("unknown case");
    std::cout<<"async PTX "<<mode<<" passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
