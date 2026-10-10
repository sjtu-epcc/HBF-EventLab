#include "hbf_eventlab/adapters/workload_reader.hpp"
#include <sstream>
#include <set>
namespace hbf_eventlab {
namespace {
std::uint64_t number(const std::string& s) {
    if(s.empty()||s.front()=='-'||s.front()=='+')throw std::invalid_argument("expected unsigned integer");
    std::size_t n=0;auto value=std::stoull(s,&n,s.rfind("0x",0)==0||s.rfind("0X",0)==0?16:10);if(n!=s.size())throw std::invalid_argument("invalid integer suffix");return value;
}
}
std::vector<GlobalRequest> read_workload(std::istream& in) {
    std::vector<GlobalRequest> out;std::string line;std::size_t lineno=0;bool header=false;
    while(std::getline(in,line)) {
        ++lineno;line=line.substr(0,line.find('#'));std::istringstream words(line);std::string op;
        if(!(words>>op))continue;
        try {
            if(!header) {
                std::string version,extra;
                if(op!="HBF_EVENTLAB_WORKLOAD"||!(words>>version)||version!="1"||(words>>extra))
                    throw std::invalid_argument("expected HBF_EVENTLAB_WORKLOAD 1 header");
                header=true;continue;
            }
            std::string id,address,bytes;
            if(!(words>>id>>address>>bytes)||(op!="READ"&&op!="WRITE"))throw std::invalid_argument("expected READ/WRITE id global_byte bytes");
            GlobalRequest r{GlobalRequestId{number(id)},op=="READ"?Op::Read:Op::Write,GlobalByteAddress{number(address)},number(bytes)};
            std::set<std::string> fields;std::string field;
            while(words>>field) {
                const auto equal=field.find('=');if(equal==std::string::npos)throw std::invalid_argument("expected key=value");
                const auto key=field.substr(0,equal),value=field.substr(equal+1);
                if(!fields.insert(key).second)throw std::invalid_argument("duplicate field");
                if(key=="content")r.content=number(value);
                else if(key=="at")r.not_before=Tick{number(value)};
                else if(key=="expect")r.expected_content=number(value);
                else if(key=="group") {
                    if(value!="none") {const auto n=number(value);if(n>UINT32_MAX)throw std::invalid_argument("group overflow");r.group=static_cast<ReadGroup>(n);}
                } else if(key=="after") {
                    if(value=="none")continue;
                    if(value.empty()||value.back()==',')throw std::invalid_argument("empty dependency");
                    std::istringstream deps(value);std::string dependency;
                    while(std::getline(deps,dependency,','))r.after.push_back(GlobalRequestId{number(dependency)});
                } else throw std::invalid_argument("unknown workload field");
            }
            if(r.op==Op::Write&&!fields.count("content"))throw std::invalid_argument("WRITE requires content identity");
            if(r.op==Op::Read&&fields.count("content"))throw std::invalid_argument("READ uses expect, not content");
            out.push_back(std::move(r));
        } catch(const std::exception& e) {throw std::invalid_argument("workload line "+std::to_string(lineno)+": "+e.what());}
    }
    if(!header)throw std::invalid_argument("missing workload version header");
    return out;
}
}

