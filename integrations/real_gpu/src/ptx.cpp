#include "hbflab/ptx.hpp"
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace hbflab {
namespace {
struct Token { std::string text; unsigned line; };
[[noreturn]] void error(unsigned line, const std::string& why) {
    throw std::invalid_argument("PTX line " + std::to_string(line) + ": " + why);
}
std::vector<Token> lex(std::string_view s) {
    if (s.empty() || s.size() > 1024*1024) error(1, "expected 1..1048576 bytes of PTX text");
    std::vector<Token> out; unsigned line=1;
    // Validate the whole image, including comments: never hide a binary suffix.
    for (const unsigned char c:s) {
        if (!c || c>127) error(line,"only ASCII PTX text is accepted (no binary/NUL)");
        if (c=='\n') ++line;
    }
    line=1;
    for (std::size_t i=0; i<s.size();) {
        const unsigned char c=s[i];
        if (!c || c>127) error(line, "only ASCII PTX text is accepted (no binary/NUL)");
        if (std::isspace(c)) { if (c=='\n') ++line; ++i; continue; }
        if (s.substr(i,2)=="//") { while (i<s.size() && s[i]!='\n') ++i; continue; }
        if (s.substr(i,2)=="/*") {
            i+=2; bool closed=false;
            while (i<s.size()) {
                if (s.substr(i,2)=="*/") { i+=2; closed=true; break; }
                if (s[i++]=='\n') ++line;
            }
            if (!closed) error(line,"unterminated comment");
            continue;
        }
        const auto start=i;
        if (std::isalnum(c) || c=='_' || c=='$' || c=='%' || c=='.') {
            do { ++i; } while (i<s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) ||
                s[i]=='_' || s[i]=='$' || s[i]=='%' || s[i]=='.'));
        } else if (std::string_view("{}()[],;:@!+-<>").find(c)!=std::string_view::npos) ++i;
        else error(line,"unsupported character");
        auto word=std::string(s.substr(start,i-start));
        if (word.find("__hbflab")!=std::string::npos) error(line,"reserved __hbflab namespace");
        out.push_back({std::move(word),line});
        if (out.size()>100000) error(line,"token budget exceeded");
    }
    out.push_back({"",line}); return out;
}
bool matches(const std::string& s, const char* pattern) {
    // Only fixed internal patterns enter this bounded per-thread cache.
    static thread_local std::map<std::string,std::regex> patterns;
    auto it=patterns.find(pattern);
    if (it==patterns.end()) it=patterns.emplace(pattern,std::regex(pattern)).first;
    return std::regex_match(s,it->second);
}
bool ident(const std::string& s) { return matches(s,"[a-zA-Z_$][a-zA-Z0-9_$]*"); }
bool reg(const std::string& s) { return matches(s,"%[a-zA-Z_$][a-zA-Z0-9_$]*"); }
bool integer(const std::string& s) { return matches(s,"(0[xX][0-9a-fA-F]+|[0-9]+)"); }
unsigned width(const std::string& type) {
    if (type==".pred") return 0;
    if (!matches(type,"\\.[busf](8|16|32|64)")) return 0;
    return static_cast<unsigned>(std::stoul(type.substr(2)))/8;
}
struct Parser {
    std::vector<Token> tokens; std::size_t pos=0; RewrittenPtx result;
    std::map<std::string,unsigned> registers, parameters;
    bool asynchronous=false;std::set<std::string> labels,targets;
    const std::string& peek() const { return tokens[pos].text; }
    unsigned line() const { return tokens[pos].line; }
    std::string take() { if (peek().empty()) error(line(),"unexpected end of input"); return tokens[pos++].text; }
    void expect(const std::string& s) { if (peek()!=s) error(line(),"expected '"+s+"', found '"+peek()+"'"); ++pos; }
    bool eat(const std::string& s) { if (peek()!=s) return false; ++pos; return true; }
    std::string register_name(unsigned bytes) {
        const auto s=take();
        if (!registers.count(s) || registers.at(s)!=bytes) error(line(),"register width/declaration mismatch: "+s);
        return s;
    }
    std::string address() {
        expect("["); const auto base=register_name(8);
        std::string op="add", offset="0";
        if (peek()=="+" || peek()=="-") {
            if (take()=="-") op="sub";
            offset=take(); if (!integer(offset)) error(line(),"address offset must be an unsigned integer literal");
            try { (void)std::stoull(offset,nullptr,offset.size()>2 && (offset[1]=='x' || offset[1]=='X')?16:10); }
            catch (...) { error(line(),"address offset overflow"); }
        }
        expect("]");
        return op+".u64 %__hbflab_addr, "+base+", "+offset+";\n";
    }
    std::string destinations(unsigned count, unsigned bytes) {
        std::string text;
        if (count>1) { expect("{"); text="{"; }
        for (unsigned i=0; i<count; ++i) {
            if (i) { expect(","); text+=", "; }
            // PTX allows 8/16-bit integer load/store operands in wider registers.
            const auto s=take();
            if (!registers.count(s) || registers[s]<bytes || registers[s]>8 || !reg(s))
                error(line(),"invalid memory operand register: "+s);
            text+=s;
        }
        if (count>1) { expect("}"); text+="}"; }
        return text;
    }
    std::string async_arguments() const {
        std::string out=".param .align 8 .b8 __hbflab_v[64];\n.param .b64 __hbflab_o, __hbflab_s, __hbflab_i, __hbflab_nb, __hbflab_nn;\n";
        for(unsigned i=0;i<64;i+=8)out+="ld.param.u64 %__hbflab_word, [__hbflab_view+"+std::to_string(i)+"];\nst.param.u64 [__hbflab_v+"+std::to_string(i)+"], %__hbflab_word;\n";
        for(const auto& p:{std::pair<const char*,const char*>{"__hbflab_o","__hbflab_output"},{"__hbflab_s","__hbflab_size"},{"__hbflab_i","__hbflab_instance"},{"__hbflab_nb","__hbflab_native"},{"__hbflab_nn","__hbflab_native_size"}})
            out+="ld.param.u64 %__hbflab_word, ["+std::string(p.second)+"];\nst.param.u64 ["+p.first+"], %__hbflab_word;\n";
        return out;
    }
    std::string async_check() const {
        return "{\n.param .b64 __hbflab_a;\n.param .b32 __hbflab_n, __hbflab_active, __hbflab_op, __hbflab_result;\n"+async_arguments()+
          "st.param.b64 [__hbflab_a], 0;\nst.param.b32 [__hbflab_n], 0;\nst.param.b32 [__hbflab_active], 0;\nst.param.b32 [__hbflab_op], 2;\n"
          "call (__hbflab_result), __hbflab_async_gate, (__hbflab_v, __hbflab_a, __hbflab_n, __hbflab_active, __hbflab_op, __hbflab_o, __hbflab_s, __hbflab_i, __hbflab_nb, __hbflab_nn);\n"
          "ld.param.b32 %__hbflab_ok, [__hbflab_result];\nsetp.eq.u32 %__hbflab_p, %__hbflab_ok, 0;\n@%__hbflab_p ret;\n}\n";
    }
    std::string memory(const std::string& opcode, const std::string& predicate, PtxEntry& entry,
                       unsigned source_line, unsigned instruction) {
        static const std::regex memory_op("(ld|st)\\.global(?:\\.(v2|v4))?\\.([bus](8|16|32|64)|f(32|64))");
        std::smatch m;
        if (!std::regex_match(opcode,m,memory_op)) error(line(),"unsupported memory opcode: "+opcode);
        const bool store=m[1]=="st";
        const unsigned count=m[2]=="v2"?2:m[2]=="v4"?4:1;
        const unsigned bytes=width("."+m[3].str());
        if (count*bytes>16) error(line(),"memory instruction exceeds 16 bytes");
        std::string regs, addr;
        if (store) { addr=address(); expect(","); regs=destinations(count,bytes); ++entry.stores; }
        else { regs=destinations(count,bytes); expect(","); addr=address(); ++entry.loads; }
        expect(";");
        entry.accesses.push_back({source_line,instruction,count*bytes,opcode,predicate,store});
        const auto guarded=predicate.empty()?std::string{}:predicate+" ";
        std::ostringstream o;
        o << "{\n.param .b64 __hbflab_a;\n.param .b32 __hbflab_n, __hbflab_active, __hbflab_op, __hbflab_result;\n";
        if(asynchronous)o<<async_arguments();
        o << "mov.u32 %__hbflab_active, 0;\n" << guarded << "mov.u32 %__hbflab_active, 1;\n"
          << "mov.u64 %__hbflab_addr, 0;\n" << guarded << addr
          << "st.param.b64 [__hbflab_a], %__hbflab_addr;\n"
          << "st.param.b32 [__hbflab_n], " << count*bytes << ";\n"
          << "st.param.b32 [__hbflab_active], %__hbflab_active;\n"
          << "st.param.b32 [__hbflab_op], " << (store?1:0) << ";\n";
        if(asynchronous)o<<"call (__hbflab_result), __hbflab_async_gate, (__hbflab_v, __hbflab_a, __hbflab_n, __hbflab_active, __hbflab_op, __hbflab_o, __hbflab_s, __hbflab_i, __hbflab_nb, __hbflab_nn);\n";
        else o<<"call.uni (__hbflab_result), __hbflab_gate, (__hbflab_a, __hbflab_n, __hbflab_active, __hbflab_op);\n";
        o << "ld.param.b32 %__hbflab_ok, [__hbflab_result];\n"
          << "setp.ne.u32 %__hbflab_p, %__hbflab_ok, 0;\n";
        if(asynchronous)o<<"@!%__hbflab_p ret;\n";
        o << "setp.ne.u32 %__hbflab_q, %__hbflab_active, 0;\n"
          << "and.pred %__hbflab_p, %__hbflab_p, %__hbflab_q;\n"
          << "@%__hbflab_p " << opcode << " "
          << (store?"[%__hbflab_addr], "+regs:regs+", [%__hbflab_addr]") << ";\n}\n";
        return o.str();
    }
    void entry() {
        registers.clear(); parameters.clear();labels.clear();targets.clear();
        eat(".visible"); expect(".entry"); PtxEntry e; e.name=take();
        if (!ident(e.name)) error(line(),"invalid entry name");
        for (const auto& prior:result.entries) if (prior.name==e.name) error(line(),"duplicate entry");
        expect("("); std::string declaration=".visible .entry "+e.name+"(";
        if(asynchronous)declaration+=".param .align 8 .b8 __hbflab_view[64], .param .u64 __hbflab_output, .param .u64 __hbflab_size, .param .u64 __hbflab_instance, .param .u64 __hbflab_native, .param .u64 __hbflab_native_size";
        if(asynchronous && peek()!=")")declaration+=", ";
        if (peek()!=")") for (;;) {
            expect(".param"); const auto type=take(); const auto name=take(); const auto bytes=width(type);
            if ((type!=".u32" && type!=".b32" && type!=".u64" && type!=".b64") || !ident(name) || parameters.count(name))
                error(line(),"parameters must be unique scalar .u32/.b32/.u64/.b64");
            if (parameters.size()>=(asynchronous?59u:64u)) error(line(),"parameter budget exceeded");
            parameters[name]=bytes; e.parameter_bytes.push_back(bytes);
            declaration+=".param "+type+" "+name;
            if (!eat(",")) break; declaration+=", ";
        }
        expect(")"); expect("{"); declaration+=") {\n";
        unsigned register_count=0;
        while (eat(".reg")) {
            const auto type=take(); const auto bytes=width(type);
            if ((!bytes && type!=".pred") || type==".f8" || type==".f16") error(line(),"unsupported register type");
            declaration+=".reg "+type+" ";
            for (;;) {
                const auto name=take(); if (!reg(name)) error(line(),"invalid register name");
                unsigned count=1; bool array=eat("<");
                if (array) {
                    const auto n=take(); if (!matches(n,"[1-9][0-9]{0,3}")) error(line(),"invalid register count");
                    count=static_cast<unsigned>(std::stoul(n)); expect(">");
                }
                register_count+=count; if (register_count>4096) error(line(),"register budget exceeded");
                for (unsigned i=0;i<count;++i) {
                    const auto actual=array?name+std::to_string(i):name;
                    if (!registers.emplace(actual,bytes).second) error(line(),"duplicate register");
                }
                declaration+=name+(array?"<"+std::to_string(count)+">":"");
                if (!eat(",")) break; declaration+=", ";
            }
            expect(";"); declaration+=";\n";
        }
        declaration+=".reg .b64 %__hbflab_addr;\n.reg .b32 %__hbflab_active, %__hbflab_ok;\n.reg .pred %__hbflab_p, %__hbflab_q;\n";
        if(asynchronous)declaration+=".reg .b64 %__hbflab_word;\n";
        unsigned instructions=0; bool returned=false;
        static const std::regex arithmetic(
            "(mov\\.(b16|b32|b64|u16|u32|u64|s16|s32|s64|f32|f64)|"
            "(add|sub)\\.(u32|u64|s32|s64|f32|f64)|"
            "(mul|mad)\\.(lo|wide)\\.(u32|s32)|"
            "(and|or|xor|not)\\.(b32|b64|pred)|"
            "(shl|shr)\\.(b32|b64|u32|u64|s32|s64)|"
            "cvt\\.(u32\\.u64|u64\\.u32)|cvta\\.to\\.global\\.u64|"
            "setp\\.(eq|ne|lt|le|gt|ge)\\.(u32|u64|s32|s64)|"
            "selp\\.(b16|b32|b64|u16|u32|u64|s32|s64|f32|f64))");
        static const std::set<std::string> special{"%tid.x","%ntid.x","%ctaid.x","%nctaid.x","%laneid","%warpid","%globaltimer"};
        while (peek()!="}") {
            if (!asynchronous && returned) error(line(),"ret must be the final instruction");
            if(asynchronous && pos+1<tokens.size() && tokens[pos+1].text==":") {
                const auto label=take();expect(":");
                if(!ident(label) || !labels.insert(label).second)error(line(),"invalid/duplicate label");
                declaration+=label+":\n";continue;
            }
            if (++instructions>4096) error(line(),"instruction budget exceeded");
            const auto source_line=line();
            std::string predicate;
            if (eat("@")) { predicate="@"; if (eat("!")) predicate+="!"; predicate+=register_name(0); }
            const auto opcode=take();
            if (opcode=="ret") {
                if (!asynchronous && !predicate.empty()) error(line(),"predicated/early return would break the collective");
                expect(";"); declaration+=predicate+" ret;\n"; returned=true;
            } else if (asynchronous && (opcode=="bra" || opcode=="bra.uni")) {
                const auto target=take();if(!ident(target))error(line(),"invalid branch target");expect(";");targets.insert(target);
                declaration+=async_check()+predicate+" "+opcode+" "+target+";\n";
            } else if (opcode.rfind("ld.global",0)==0 || opcode.rfind("st.global",0)==0) {
                declaration+=memory(opcode,predicate,e,source_line,instructions);
            } else if (matches(opcode,"ld\\.param\\.[bu](32|64)")) {
                if (!predicate.empty()) error(line(),"predicated parameter load is outside this subset");
                const auto bytes=width(opcode.substr(8)); const auto dest=register_name(bytes);
                expect(","); expect("["); const auto param=take(); expect("]"); expect(";");
                if (!parameters.count(param) || parameters[param]!=bytes) error(line(),"parameter width/name mismatch");
                declaration+=opcode+" "+dest+", ["+param+"];\n";
            } else {
                if (!std::regex_match(opcode,arithmetic) && !(asynchronous && opcode=="mul.f32")) error(line(),"unsupported instruction/control flow: "+opcode);
                declaration+=predicate+" "+opcode+" ";
                bool operand=true;
                unsigned operands=0;
                while (peek()!=";") {
                    if (!operand) { expect(","); declaration+=", "; operand=true; continue; }
                    if (peek()=="-" || peek()=="+") {
                        if (!operands) error(source_line,"destination cannot have a sign");
                        declaration+=take();
                    }
                    const auto value=take();
                    if (!operands && !registers.count(value)) error(source_line,"destination must be a declared register");
                    if (!registers.count(value) && !special.count(value) && !integer(value) &&
                        !matches(value,"0[fFdD][0-9a-fA-F]+")) error(line(),"unsupported operand: "+value);
                    declaration+=value; operand=false; ++operands;
                }
                if (operand) error(line(),"missing operand");
                const auto base=opcode.substr(0,opcode.find('.'));
                const unsigned required=base=="mad" || base=="selp"?4:
                    base=="mov" || base=="not" || base=="cvt" || base=="cvta"?2:3;
                if (operands!=required) error(source_line,"wrong operand count for "+opcode);
                expect(";"); declaration+=";\n";
            }
        }
        expect("}");
        for(const auto& target:targets)if(!labels.count(target))error(line(),"undefined branch target");
        if(asynchronous)declaration+="ret;\n";
        if (!returned || !e.loads) error(line(),"entry requires at least one global load and one final ret");
        result.text+=declaration+"}\n"; result.entries.push_back(std::move(e));
    }
    RewrittenPtx run() {
        expect(".version"); const auto version=take();
        if (!matches(version,"(7|8)\\.[0-9]|9\\.0")) error(line(),"supported PTX version range: 7.x..9.0");
        expect(".target"); const auto target=take();
        if (!matches(target,"sm_(75|80|86|87|88|89|90|100|103|110|120|121)")) error(line(),"unsupported target (SM75+ without architecture suffixes)");
        result.target_sm=static_cast<unsigned>(std::stoul(target.substr(3)));
        expect(".address_size"); expect("64");
        result.text=".version "+version+"\n.target "+target+"\n.address_size 64\n"
            ".extern .func (.param .b32 result) __hbflab_gate(.param .b64 address, .param .b32 bytes, .param .b32 active, .param .b32 op);\n";
        if(asynchronous) {
            const auto at=result.text.find(".extern");result.text.resize(at);
            result.text+=".extern .func (.param .b32 result) __hbflab_async_gate(.param .align 8 .b8 view[64], .param .b64 address, .param .b32 bytes, .param .b32 active, .param .b32 op, .param .b64 output, .param .b64 size, .param .b64 instance, .param .b64 native, .param .b64 native_size);\n";
        }
        while (!peek().empty()) {
            if (result.entries.size()>=64) error(line(),"entry budget exceeded");
            entry();
        }
        if (result.entries.empty()) error(line(),"no entries");
        return result;
    }
};
}
RewrittenPtx rewrite_ptx(std::string_view source) { return Parser{lex(source)}.run(); }
RewrittenPtx rewrite_async_ptx(std::string_view source) {Parser p{lex(source)};p.asynchronous=true;return p.run();}
}
