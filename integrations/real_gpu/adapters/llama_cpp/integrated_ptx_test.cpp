#include "hbflab/async_module.hpp"
#include "weight_kernel_embedded.hpp"
#include <cuda_runtime_api.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <regex>
#include <map>
#include <cstring>
#include <iostream>
#include <sstream>
using namespace hbflab;
void need(bool b,const char* message){if(!b)throw std::runtime_error(message);}
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}

// Add timers to the actual NVCC entry without changing its accesses/arithmetic.
// The measured interval includes both original input loads and ends at the output store.
std::string timed_source(){
    std::string source=embedded_weight_kernel;
    const auto start=source.find('{');need(start!=std::string::npos,"PTX entry missing");
    source.insert(start+1,"\n.reg .b64 %m6_begin, %m6_end, %m6_base, %m6_delta, %m6_ptr;\n");
    std::smatch parameter;
    need(std::regex_search(source,parameter,std::regex(R"(ld\.param\.u64\s+(%\w+),\s*\[m6_weight_mul_param_2\];)")),"output parameter missing");
    const std::string output_register=parameter[1];
    source.insert(parameter.position()+parameter.length(),"\nmov.u64 %m6_base, "+output_register+";\n");
    // Each basic block containing loads has one store. Restart the interval after each store.
    std::istringstream input(source);std::string line,result;bool started=false;unsigned stores=0;
    const std::regex store(R"(st\.global\.f32\s+\[(%\w+)\],)");
    while(std::getline(input,line)){
        if(line.find("ld.global.f32")!=std::string::npos&&!started){result+="mov.u64 %m6_begin, %globaltimer;\n";started=true;}
        std::smatch match;
        if(std::regex_search(line,match,store)){
            need(started,"store without measured loads");
            // Timer array follows an 8 KiB aligned output area; byte offset * 2 gives u64 indexing.
            result+="mov.u64 %m6_end, %globaltimer;\nsub.u64 %m6_delta, %m6_end, %m6_begin;\n";
            result+="sub.u64 %m6_ptr, "+std::string(match[1])+", %m6_base;\nshl.b64 %m6_ptr, %m6_ptr, 1;\nadd.u64 %m6_ptr, %m6_ptr, %m6_base;\nadd.u64 %m6_ptr, %m6_ptr, 32768;\nst.global.u64 [%m6_ptr], %m6_delta;\n";
            started=false;++stores;
        }
        result+=line+'\n';
    }
    need(stores==2,"unexpected NVCC layout: inspect timer instrumentation");return result;
}

int main(int argc,char** argv)try{
    need(argc==3,"width and output required");unsigned width=static_cast<unsigned>(std::stoul(argv[1]));need(width>=32&&width<=8192,"width");
    const std::filesystem::path output=argv[2];std::filesystem::create_directories(output);
    std::vector<float> x(width),w(width),y(width);for(unsigned i=0;i<width;++i){x[i]=float(int(i%23)-11)/8;w[i]=float(int(i%19)-9)/16;}
    float *dx=nullptr,*dw=nullptr,*dy=nullptr;ck(cudaMalloc((void**)&dx,width*4));ck(cudaMalloc((void**)&dw,width*4));ck(cudaMalloc((void**)&dy,32768+width*8));
    ck(cudaMemcpy(dx,x.data(),width*4,cudaMemcpyHostToDevice));ck(cudaMemcpy(dw,w.data(),width*4,cudaMemcpyHostToDevice));ck(cudaMemset(dy,0,32768+width*8));
    AsyncRuntimeOptions options;options.time_scale=100;options.audit_limit=width+1;options.backend.max_events_per_batch=10000000;options.service_timeout_ns=10000000000ull;
    LiveTimingService service(options,512,2);service.register_readonly(dw,width*4);service.start();
    AsyncModuleInterceptor loader(service);const auto source=timed_source();auto module=loader.load_ptx(source);auto function=loader.function(module,"m6_weight_mul");
    loader.launch(function,{2,128},{&dx,&dw,&dy,&width},{dy,32768+width*8},nullptr,{dx,width*4});loader.unload(module);
    ck(cudaMemcpy(y.data(),dy,width*4,cudaMemcpyDeviceToHost));std::vector<std::uint64_t> elapsed(width);ck(cudaMemcpy(elapsed.data(),reinterpret_cast<char*>(dy)+32768,width*8,cudaMemcpyDeviceToHost));
    auto audit=service.take_audit();need(audit.size()==width,"PTX count");std::vector<bool> seen(width);std::uint64_t minimum=~std::uint64_t(0);
    for(const auto& t:audit){const auto offset=t.first_address-reinterpret_cast<std::uintptr_t>(dw);need(offset%4==0&&offset/4<width,"weight bounds");const auto i=offset/4;need(!seen[i],"duplicate weight");seen[i]=true;
        const auto delay=((t.end_ps-t.arrival_ps)*options.time_scale+999)/1000;need(elapsed[i]>=delay,"actual M6 PTX load bypassed target wait");minimum=std::min(minimum,elapsed[i]-delay);}
    for(unsigned i=0;i<width;++i)need(y[i]==x[i]*w[i],"PTX multiply oracle");service.finish();need(service.drained()&&service.statistics().submitted==width&&service.statistics().completed==width,"PTX drain");
    std::ofstream(output/"timed.ptx")<<source;
    std::ofstream(output/"validation.json")<<nlohmann::json({{"status","pass"},{"width",width},{"requests",audit.size()},{"time_scale",options.time_scale},{"minimum_wait_margin_ns",minimum},{"original_ptx_sha256",sha256(embedded_weight_kernel)},{"timed_ptx_sha256",sha256(source)},{"all_outputs_exact",true},{"drained",true}}).dump(2);
    ck(cudaFree(dx));ck(cudaFree(dw));ck(cudaFree(dy));std::cout<<"M6 width "<<width<<" multiply and device timers passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
