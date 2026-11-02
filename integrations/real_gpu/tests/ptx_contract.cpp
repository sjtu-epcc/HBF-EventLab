#include "hbflab/ptx.hpp"
#include "ptx_fixture.hpp"
#include <iostream>
#include <chrono>

using namespace hbflab;
int main(int argc,char** argv) try {
    const std::string mode=argc>1?argv[1]:"coverage";
    if (mode=="hash") {
        require(sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","empty hash");
        require(sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","abc hash");
        require(sha256(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","multiblock hash");
    } else if (mode=="coverage") {
        for (const auto type:{"u8","s8","b8","u16","s16","b16","u32","s32","b32","u64","s64","b64","f32","f64"}) {
            for (unsigned count:{1u,2u,4u}) {
                const auto bytes=std::stoul(std::string(type).substr(1))/8;
                std::string regs=bytes==8?"%rd5":"%r1";
                if (count>1) { regs="{"; for (unsigned i=0;i<count;++i) { if (i) regs+=","; regs+=(bytes==8?"%rd":"%r")+std::to_string(i+5); } regs+="}"; }
                const auto op=std::string("ld.global.")+(count==1?"":"v"+std::to_string(count)+".")+type;
                auto source=ptx_fixture(op+" "+regs+", [%rd2+16];");
                if (bytes*count>16) rejects([&]{rewrite_ptx(source);});
                else require(rewrite_ptx(source).entries[0].loads==1,"coverage count");
            }
        }
    } else if (mode=="predicate") {
        for (const auto guard:{"@%p0","@!%p0"}) {
            const auto r=rewrite_ptx(ptx_fixture(std::string(guard)+" ld.global.u32 %r1,[%rd2];"));
            require(r.text.find(std::string(guard)+" mov.u32 %__hbflab_active, 1;")!=std::string::npos,"predicate not captured");
            require(r.text.find("\ncall.uni (")!=std::string::npos,"collective call must be unpredicated");
            require(r.text.find("@%__hbflab_p ld.global.u32")!=std::string::npos,"load not gated");
        }
    } else if (mode=="reject_control") {
        for (const auto body:{"bra L; L: ld.global.u32 %r1,[%rd2];", "@%p0 ret; ld.global.u32 %r1,[%rd2];",
            "ret; ld.global.u32 %r1,[%rd2];", "call foo;", "bar.sync 0;", "exit;", "trap;", "{ ld.global.u32 %r1,[%rd2]; }"})
            rejects([&]{rewrite_ptx(ptx_fixture(body));});
    } else if (mode=="reject_memory") {
        for (const auto op:{"ld.u32","ld.shared.u32","ld.const.u32","ld.local.u32","ld.global.nc.u32","ld.global.ca.u32",
            "ld.volatile.global.u32","ld.acquire.gpu.global.u32","ld.global.v8.u32","atom.global.add.u32","red.global.add.u32",
            "cp.async.ca.shared.global","ldmatrix.sync.aligned.m8n8.x1.shared.b16","st.global.release.u32"})
            rejects([&]{rewrite_ptx(ptx_fixture(std::string(op)+" %r1,[%rd2];"));});
    } else if (mode=="reject_module") {
        for (const auto extra:{".extern .func foo();", ".global .u32 value;", ".file 1 \"a\"", ".entry x(){ret;}", ".pragma \"nounroll\";"})
            rejects([&]{rewrite_ptx(ptx_fixture()+extra);});
        auto source=ptx_fixture(); replace_one(source,".address_size 64",".address_size 32"); rejects([&]{rewrite_ptx(source);});
        source=ptx_fixture(); replace_one(source,"%r1","%__hbflab_ok"); rejects([&]{rewrite_ptx(source);});
        rejects([&]{rewrite_ptx(std::string("\x7f" "ELF",4));});
        rejects([&]{rewrite_ptx(ptx_fixture()+std::string(1,'\0'));});
    } else if (mode=="syntax") {
        for (const auto body:{"ld.global.u32 %r1,[%rd2+%rd4];", "ld.global.u32 %r1,[%r0];", "@%r0 ld.global.u32 %r1,[%rd2];",
            "ld.global.u32 %r1,[%rd2]; /*", "ld.param.u64 %rd0,[missing]; ld.global.u32 %r1,[%rd2];",
            "ld.global.u32 %r1,[%rd2]; unsupported;"})
            rejects([&]{rewrite_ptx(ptx_fixture(body));});
    } else if (mode=="multiple") {
        auto source=ptx_fixture(); auto other=ptx_fixture("ld.global.u64 %rd5,[%rd2];","other");
        source+=other.substr(other.find(".visible")); const auto r=rewrite_ptx(source);
        require(r.entries.size()==2 && r.entries[1].name=="other","module entries lost");
        rejects([&]{rewrite_ptx(source+source.substr(source.find(".visible")));});
    } else if (mode=="limits") {
        rejects([&]{rewrite_ptx(std::string(1024*1024+1,' '));});
        auto source=ptx_fixture(); replace_one(source,"%r<16>","%r<9999>"); rejects([&]{rewrite_ptx(source);});
        source=ptx_fixture(); replace_one(source,"[%rd2]","[%rd2+18446744073709551616]"); rejects([&]{rewrite_ptx(source);});
    } else if (mode=="arity") {
        for (const auto bad:{"add.u32 %r1,%r2;", "mov.u32 %r1,%r2,%r3;", "mad.lo.u32 %r1,%r2,%r3;",
            "selp.u32 %r1,%r2,%p0;", "not.b32 %r1;", "mov.u32 %tid.x,%r0;", "add.u32 1,%r0,%r1;", "mov.u32 -%r1,1;"})
            rejects([&]{rewrite_ptx(ptx_fixture(std::string(bad)+"ld.global.u32 %r1,[%rd2];"));});
    } else if (mode=="binary_comments") {
        for (auto suffix:{std::string("//hidden\0binary",15),std::string("/*hidden\0binary*/",17),std::string("//\xff",3)})
            rejects([&]{rewrite_ptx(ptx_fixture()+suffix);});
    } else if (mode=="access_manifest") {
        const auto source=ptx_fixture("setp.eq.u32 %p0,%r0,1;\n@!%p0 ld.global.v2.u32 {%r1,%r2},[%rd2];\nst.global.u32 [%rd3],%r1;");
        const auto result=rewrite_ptx(source); const auto& sites=result.entries[0].accesses;
        const auto at=source.find("@!%p0 ld.global");
        unsigned line=1; for (std::size_t i=0;i<at;++i) if (source[i]=='\n') ++line;
        require(sites.size()==2 && sites[0].source_line==line && sites[0].bytes==8 && !sites[0].store &&
            sites[0].predicate=="@!%p0" && sites[0].opcode=="ld.global.v2.u32","load site attribution");
        require(sites[1].source_line==line+1 && sites[1].store && sites[1].instruction==sites[0].instruction+1,"store site attribution");
    } else if (mode=="large") {
        std::string body; for (unsigned i=0;i<4000;++i) body+="add.u32 %r1,%r0,1;\n";
        body+="ld.global.u32 %r1,[%rd2];";
        const auto begin=std::chrono::steady_clock::now(); const auto result=rewrite_ptx(ptx_fixture(body));
        require(result.entries[0].accesses[0].instruction==4007,"large instruction ordinal");
        std::cout<<"4008 instructions rewrite_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<"\n";
        for (unsigned i=0;i<100;++i) body+="add.u32 %r1,%r0,1;\n";
        rejects([&]{rewrite_ptx(ptx_fixture(body));});
    } else throw std::invalid_argument("unknown test");
    std::cout<<"PASS ptx "<<mode<<"\n"; return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
