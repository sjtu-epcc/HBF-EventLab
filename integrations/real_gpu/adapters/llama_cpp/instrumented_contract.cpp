#include "instrumented_bridge.hpp"
#include <cuda_runtime_api.h>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <string>
#include <chrono>
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
int main(int argc,char** argv)try {
    if(argc!=3)throw std::runtime_error("case and output root required");
    const std::string mode=argv[1];
    const auto out=std::filesystem::path(argv[2])/(mode+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(out);
    float* norm=nullptr;void* down=nullptr;ck(cudaMalloc(reinterpret_cast<void**>(&norm),4096));ck(cudaMalloc(&down,3072*1024*2));
    ck(cudaMemset(norm,0,4096));ck(cudaMemset(down,0,6144));
    ggml_tensor n{},w{};
    n.type=GGML_TYPE_F32;n.data=norm;w.type=GGML_TYPE_F16;w.data=down;
    for(auto* t:{&n,&w}) {
        t->ne[0]=t==&n?1024:3072;t->ne[1]=t==&n?1:1024;t->ne[2]=t->ne[3]=1;
        t->nb[0]=t==&n?4:2;for(int i=1;i<4;++i)t->nb[i]=t->nb[i-1]*t->ne[i-1];
    }
    std::strcpy(n.name,"blk.10.ffn_norm.weight");std::strcpy(w.name,"blk.10.ffn_down.weight");
    bool rejected=false;
    hbflab_m2_configure("bypass",out.string().c_str());
    std::cerr<<"configured\n";
    try {
        if(mode=="layout")w.type=GGML_TYPE_F32;
        std::cerr<<"binding\n";hbflab_m2_bind(&n,&w);std::cerr<<"bound\n";hbflab_m2_step_begin(0);
        ggml_tensor node{};node.op=GGML_OP_MUL_MAT;node.src[0]=&w;
        if(mode=="alias")std::strcpy(w.name,"unnamed-overlapping-view");
        if(mode=="moved")w.data=norm;
        if(mode=="operator")node.op=GGML_OP_ADD;
        if(mode=="missing")hbflab_m2_step_end();
        else hbflab_m2_graph_node(&node);
    }catch(const std::exception& e){rejected=true;std::cout<<e.what()<<'\n';}
    std::cerr<<"aborting\n";hbflab_m2_abort();ck(cudaFree(norm));ck(cudaFree(down));
    if(!rejected)throw std::runtime_error("expected strict rejection");
    std::cout<<"contract "<<mode<<" passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
