#include "hbflab/module.hpp"
#include <iostream>
#include <stdexcept>

void check(cudaError_t error) { if (error!=cudaSuccess) throw std::runtime_error(cudaGetErrorString(error)); }
int main(int argc,char** argv) try {
    hbflab::RuntimeOptions options; options.threads=45; options.mode=hbflab::ProducerMode::Warp;
    hbflab::Runtime runtime(options);
    std::vector<std::uint32_t> input(options.threads);
    for (unsigned i=0;i<input.size();++i) input[i]=i*13;
    const auto* gpu_input=runtime.upload(input.data(),input.size()*sizeof(input[0]));
    struct Output {
        std::uint32_t* data=nullptr;
        ~Output() { cudaFree(data); }
    } output;
    const auto bytes=input.size()*sizeof(input[0]);
    check(cudaMalloc(reinterpret_cast<void**>(&output.data),bytes));
    hbflab::ModuleInterceptor interceptor(runtime);
    const auto module=interceptor.load_file(argc>1?argv[1]:HBFLAB_EXAMPLE_PTX);
    const auto function=interceptor.function(module,"read_and_add");
    const auto status=interceptor.launch(function,1,options.threads,{&gpu_input,&output.data},{output.data,bytes});
    if (status!=hbflab::Status::Ok) throw std::runtime_error("PTX timing session failed");
    std::vector<std::uint32_t> result(input.size());
    check(cudaMemcpy(result.data(),output.data,bytes,cudaMemcpyDeviceToHost));
    for (unsigned i=0;i<result.size();++i) if (result[i]!=input[i]+7) throw std::runtime_error("GPU output mismatch");
    const auto identity=interceptor.identity(module);
    std::cout<<"PTX file -> rewrite -> JIT -> EventLab -> GPU result: PASS\n"
        <<"threads="<<options.threads<<" requests="<<runtime.statistics().requests<<" sectors="<<runtime.statistics().sectors
        <<"\nartifact="<<identity.artifact_sha256<<"\ncubin="<<identity.cubin_sha256<<"\n";
    interceptor.unload(module);
    return 0;
} catch (const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
