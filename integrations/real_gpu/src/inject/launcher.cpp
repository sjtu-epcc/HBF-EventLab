#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <detours.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){if(value)CloseHandle(value);}
};
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(std::string(what)+" (Win32 "+std::to_string(GetLastError())+")");}
std::wstring quote(const std::wstring& arg) {
    std::wstring out=L"\""; unsigned slashes=0;
    for(wchar_t c:arg) {
        if(c==L'\\'){++slashes;continue;}
        out.append(c==L'"'?slashes*2+1:slashes,L'\\'); out+=c; slashes=0;
    }
    out.append(slashes*2,L'\\');return out+L"\"";
}
std::uint64_t integer(const std::wstring& word,std::uint64_t max) {
    if(word.empty() || word.find_first_not_of(L"0123456789")!=std::wstring::npos)throw std::invalid_argument("expected unsigned integer");
    const auto value=std::stoull(word); if(value>max)throw std::invalid_argument("injection option exceeds limit");return value;
}
void role(const std::wstring& value,const wchar_t* kind,std::map<std::wstring,std::wstring>& env) {
    const auto split=value.find(L':'); if(split==std::wstring::npos)throw std::invalid_argument("role must be ARG_INDEX:BYTES");
    const auto index=value.substr(0,split),bytes=value.substr(split+1);
    integer(index,63); if(!integer(bytes,1024*1024*1024ull))throw std::invalid_argument("zero role bytes");
    env[std::wstring(L"HBFLAB_INJECT_")+kind+L"_ARG"]=index;
    env[std::wstring(L"HBFLAB_INJECT_")+kind+L"_BYTES"]=bytes;
}
std::vector<wchar_t> environment(const std::map<std::wstring,std::wstring>& settings) {
    std::vector<std::wstring> entries;
    const auto block=GetEnvironmentStringsW(); check(block!=nullptr,"read environment");
    for(const wchar_t* p=block;*p;p+=wcslen(p)+1) {
        const std::wstring entry=p;
        if(_wcsnicmp(entry.c_str(),L"HBFLAB_INJECT_",14)!=0)entries.push_back(entry);
    }
    FreeEnvironmentStringsW(block);
    for(const auto& e:settings) entries.push_back(e.first+L"="+e.second);
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
    std::vector<wchar_t> result;
    for(const auto& e:entries){result.insert(result.end(),e.begin(),e.end());result.push_back(0);}
    result.push_back(0);return result;
}
}
int wmain(int argc,wchar_t** argv) try {
    if(argc==1 || std::wstring(argv[1])==L"--help") {
        std::cout<<"hbflab_inject --entry NAME --input INDEX:BYTES --output INDEX:BYTES --threads N --mode thread|warp -- TARGET.exe [args]\n"
            <<"Optional: --protocol collective|async --slots N --launches N --native INDEX:BYTES\n"
            <<"Async requires thread mode; native is an explicit unmodeled read span.\n"
            <<"Windows x64 startup injection, CUDA Driver primary context, immutable input while modules live.\n";
        return argc==1?2:0;
    }
    std::map<std::wstring,std::wstring> settings{{L"HBFLAB_INJECT_ACTIVE",L"1"},{L"HBFLAB_INJECT_PROTOCOL",L"collective"},{L"HBFLAB_INJECT_SLOTS",L"512"},{L"HBFLAB_INJECT_LAUNCHES",L"2"}};
    int target=0;
    for(int i=1;i<argc;++i) {
        const std::wstring name=argv[i];
        if(name==L"--"){target=i+1;break;}
        if(++i==argc)throw std::invalid_argument("missing option value"); const std::wstring value=argv[i];
        if(name==L"--entry")settings[L"HBFLAB_INJECT_ENTRY"]=value;
        else if(name==L"--input")role(value,L"INPUT",settings);
        else if(name==L"--output")role(value,L"OUTPUT",settings);
        else if(name==L"--native")role(value,L"NATIVE",settings);
        else if(name==L"--threads") {
            if(!integer(value,256))throw std::invalid_argument("zero threads");settings[L"HBFLAB_INJECT_THREADS"]=value;
        } else if(name==L"--mode") {
            if(value!=L"thread" && value!=L"warp")throw std::invalid_argument("mode must be thread or warp");settings[L"HBFLAB_INJECT_MODE"]=value;
        } else if(name==L"--protocol") {
            if(value!=L"collective" && value!=L"async")throw std::invalid_argument("protocol must be collective or async");settings[L"HBFLAB_INJECT_PROTOCOL"]=value;
        } else if(name==L"--slots" || name==L"--launches") {
            if(!integer(value,name==L"--slots"?8192:256))throw std::invalid_argument("zero pool limit");settings[name==L"--slots"?L"HBFLAB_INJECT_SLOTS":L"HBFLAB_INJECT_LAUNCHES"]=value;
        } else throw std::invalid_argument("unknown injection option");
    }
    const bool native=settings.count(L"HBFLAB_INJECT_NATIVE_ARG")!=0;
    if(settings.size()!=(native?13u:11u) || !target || target>=argc)throw std::invalid_argument("all injection options and target after -- are required");
    if(native && (settings.at(L"HBFLAB_INJECT_PROTOCOL")!=L"async" || integer(settings.at(L"HBFLAB_INJECT_NATIVE_ARG"),63)==integer(settings.at(L"HBFLAB_INJECT_INPUT_ARG"),63) || integer(settings.at(L"HBFLAB_INJECT_NATIVE_ARG"),63)==integer(settings.at(L"HBFLAB_INJECT_OUTPUT_ARG"),63)))throw std::invalid_argument("native read role requires async and distinct argument index");
    if(settings.at(L"HBFLAB_INJECT_PROTOCOL")==L"async" && (settings.at(L"HBFLAB_INJECT_MODE")!=L"thread" || integer(settings.at(L"HBFLAB_INJECT_SLOTS"),8192)<integer(settings.at(L"HBFLAB_INJECT_THREADS"),256)))throw std::invalid_argument("async requires thread mode and enough exclusive slots");
    if(integer(settings.at(L"HBFLAB_INJECT_INPUT_ARG"),63)==integer(settings.at(L"HBFLAB_INJECT_OUTPUT_ARG"),63))throw std::invalid_argument("input/output argument indices must differ");
    const auto& entry=settings.at(L"HBFLAB_INJECT_ENTRY");
    if(entry.empty() || entry.size()>200 || entry.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$")!=std::wstring::npos)
        throw std::invalid_argument("invalid entry name");
    const auto executable=std::filesystem::absolute(argv[target]);
    DWORD type=0; check(GetBinaryTypeW(executable.c_str(),&type)!=0,"read target executable");
    if(type!=SCS_64BIT_BINARY)throw std::invalid_argument("target must be Windows x64");
    wchar_t own[32768];const auto length=GetModuleFileNameW(nullptr,own,32768);check(length && length<32768,"launcher path");
    const auto dll=std::filesystem::path(own).parent_path()/L"hbflab_inject.dll";
    if(!std::filesystem::is_regular_file(dll))throw std::invalid_argument("hbflab_inject.dll must be beside launcher");
    const auto dll_ansi=dll.string();
    if(std::filesystem::path(dll_ansi)!=dll)throw std::invalid_argument("DLL path must be representable in the Windows ANSI codepage");
    auto env=environment(settings);
    auto command=quote(executable.wstring());
    for(int i=target+1;i<argc;++i)command+=L" "+quote(argv[i]);
    Handle job; job.value=CreateJobObjectW(nullptr,nullptr);check(job.value!=nullptr,"create child lifetime job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    check(SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=0,"configure child job");
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    check(DetourCreateProcessWithDllExW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,env.data(),nullptr,&startup,&process,dll_ansi.c_str(),nullptr)!=0,"create injected child");
    Handle child,thread;child.value=process.hProcess;thread.value=process.hThread;
    if(!AssignProcessToJobObject(job.value,child.value)){const auto error=GetLastError();TerminateProcess(child.value,2);SetLastError(error);check(false,"assign child job");}
    check(ResumeThread(thread.value)!=static_cast<DWORD>(-1),"resume injected child");
    check(WaitForSingleObject(child.value,INFINITE)==WAIT_OBJECT_0,"wait for child");
    DWORD exit=0;check(GetExitCodeProcess(child.value,&exit)!=0,"child exit status");
    if(exit)std::cerr<<"Injected child exited with status "<<exit<<"\n";
    return static_cast<int>(exit);
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 2;}
