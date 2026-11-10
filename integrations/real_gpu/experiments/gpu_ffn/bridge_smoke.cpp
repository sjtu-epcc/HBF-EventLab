// Exercise the imported gate/auditor through the main backend, without CUDA.
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <chrono>
using json = nlohmann::ordered_json;
namespace fs = std::filesystem;
#include "gate.hpp"
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){
    try {
        require(argc==2,"Expected effective INI path");
        const auto root=fs::current_path()/("ffn-smoke-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(root),"Fresh result directory required");
        auto region=json{{"region_id","synthetic"},{"tensor","synthetic.weight"},{"source_offset",0},
            {"source_bytes",8192},{"valid_bytes",8192},{"registered_bytes",8192},{"global_base",0},{"content_id",1}};
        auto config=json{{"effective_config_path",argv[1]},{"regions",json::array({region})}};
        {
            std::uint64_t stamp=0;
            S3Gate gate(root,config,[&]{return ++stamp;});
            gate.initialize();gate.request_begin();gate.advance_segment(1234);
            auto node=json{{"node_id",1},{"op","MUL_MAT"},{"weight_accesses",json::array({
                {{"callback_id",1},{"source",{{"name","synthetic.weight"}}},{"source_offset",0},{"source_bytes",8192}}})}};
            gate.before(node,0);gate.token(42,0,false);
            const auto result=gate.finish();
            require(result.at("workload_success")&&result.at("host_drained")&&result.at("device_drained"),"Gate did not drain");
            require(result.at("read_globals")==2,"Expected two completed read Globals");
            require(result.at("online_audit").at("read_fragments")==128&&result.at("online_audit").at("write_fragments")==128,"Fragment coverage mismatch");
            require(result.at("host_attempt_errors")==0,"Unexpected attempt errors");
            require(result.at("finished_ps").get<std::uint64_t>()-result.at("request_t0_ps").get<std::uint64_t>()==1234+result.at("accumulated_wait_ps").get<std::uint64_t>(),"Clock mismatch");
        }
        const auto failure=root/"failure";fs::create_directory(failure);config["persistent_program_fail"]=true;
        {
            S3Gate gate(failure,config,[]{return std::uint64_t{0};});
            bool rejected=false;try{gate.initialize();}catch(const HbfDependencyError&){rejected=true;}
            require(rejected&&gate.initialization_failed(),"Failed initialization must block consumers");
        }
        std::cout<<"Main-backend FFN gate: initialization, reads, audit, timing, drain and failure passed\n";
        fs::remove_all(root);return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
