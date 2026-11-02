#include "integrated_profile.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include "paged_bridge.hpp"
#include <cuda_fp16.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
void need(bool b,const char* s){if(!b)throw std::runtime_error(s);}
int main(int argc,char** argv)try{
    need(argc==3||argc==4,"profile and output directory required");const std::string mode=argc==4?argv[3]:"oracle";need(mode=="oracle"||mode=="position"||mode=="shape","unknown oracle mode");std::filesystem::create_directories(argv[2]);m6_load_profile(argv[1],argv[2]);const auto& p=integrated_profile();
    const unsigned dim=p.head_dim,qh=p.q_heads,kh=p.kv_heads,qe=p.q_elements(),ke=p.kv_elements(),first_batch=17;
    float *qd,*kd,*vd,*od,*registered;ck(cudaMalloc((void**)&registered,4096));ck(cudaMemset(registered,0,4096));ck(cudaMalloc((void**)&qd,first_batch*qe*4));ck(cudaMalloc((void**)&kd,first_batch*ke*4));ck(cudaMalloc((void**)&vd,first_batch*ke*4));ck(cudaMalloc((void**)&od,first_batch*qe*4));
    int* pd;ck(cudaMalloc((void**)&pd,first_batch*4));
    m4_begin(1,nullptr,argv[2]);
    std::vector<std::vector<float>> historyk(p.layers),historyv(p.layers);double max_error=0;
    for(int step=0;step<2;++step){const int n=step?1:first_batch,past=step?first_batch:0;m4_step_begin(step);
      for(int layer=0;layer<p.layers;++layer){
        std::vector<float> q(n*qe),k(n*ke),v(k.size()),out(q.size());
        for(unsigned i=0;i<q.size();++i)q[i]=std::sin(float(i+layer*7+step*11)*.013f)*2;
        for(unsigned i=0;i<k.size();++i){k[i]=std::cos(float(i+layer*3+step*13)*.021f);v[i]=std::sin(float(i+layer*5+step*17)*.031f);historyk[layer].push_back(__half2float(__float2half(k[i])));historyv[layer].push_back(__half2float(__float2half(v[i])));}
        ck(cudaMemcpy(qd,q.data(),q.size()*4,cudaMemcpyHostToDevice));ck(cudaMemcpy(kd,k.data(),k.size()*4,cudaMemcpyHostToDevice));ck(cudaMemcpy(vd,v.data(),v.size()*4,cudaMemcpyHostToDevice));
        ggml_tensor pt{},qt{},kt{},vt{},dst{};std::vector<int> positions(n);for(int i=0;i<n;++i)positions[i]=past+i;ck(cudaMemcpy(pd,positions.data(),n*4,cudaMemcpyHostToDevice));pt.type=GGML_TYPE_I32;pt.ne[0]=n;pt.ne[1]=pt.ne[2]=pt.ne[3]=1;pt.data=pd;for(auto t:{&qt,&kt,&vt,&dst}){t->type=GGML_TYPE_F32;t->ne[0]=dim;t->ne[1]=t==&qt?qh:kh;t->ne[2]=n;t->ne[3]=1;t->nb[0]=4;for(int j=1;j<4;++j)t->nb[j]=t->nb[j-1]*t->ne[j-1];}
        qt.data=qd;kt.data=kd;vt.data=vd;dst.data=od;dst.ne[0]=qe;dst.ne[1]=n;dst.ne[2]=1;dst.nb[1]=qe*4;dst.nb[2]=dst.nb[3]=qe*4*n;dst.op=GGML_OP_CUSTOM;dst.src[0]=&qt;dst.src[1]=&kt;dst.src[2]=&vt;dst.src[3]=&pt;int tag=0x4d34564b;std::memcpy((char*)dst.op_params+56,&layer,4);std::memcpy((char*)dst.op_params+60,&tag,4);
        if(mode!="oracle"){
            if(mode=="position"){++positions[0];ck(cudaMemcpy(pd,positions.data(),n*4,cudaMemcpyHostToDevice));}else ++qt.ne[0];
            bool rejected=false;try{m4_attention(&dst,nullptr);}catch(const std::exception& e){rejected=std::string(e.what()).find(mode=="position"?"contiguous append positions":"unsupported attention shape")!=std::string::npos;}
            need(rejected,"invalid attention input was accepted");need(!std::filesystem::exists(std::filesystem::path(argv[2])/"kv-summary.json"),"failed transaction published success");m4_abort();
            for(auto allocation:{qd,kd,vd,od,registered})ck(cudaFree(allocation));ck(cudaFree(pd));
            std::ofstream(std::filesystem::path(argv[2])/"rejection.json")<<nlohmann::json({{"status","pass"},{"case",mode},{"rejected",true},{"success_not_published",true}}).dump(2);return 0;
        }
        m4_attention(&dst,nullptr);ck(cudaMemcpy(out.data(),od,out.size()*4,cudaMemcpyDeviceToHost));
        for(int r=0;r<n;++r)for(int h=0;h<qh;++h){std::vector<double> scores(past+r+1);double mx=-1e300,den=0;
          for(int t=0;t<=past+r;++t){double dot=0;for(int d=0;d<dim;++d)dot+=double(q[(r*qh+h)*dim+d])*historyk[layer][t*ke+(h/(qh/kh))*dim+d];scores[t]=dot/std::sqrt(double(dim));mx=std::max(mx,scores[t]);}
          for(auto& a:scores){a=std::exp(a-mx);den+=a;}
          for(int d=0;d<dim;++d){double acc=0;for(int t=0;t<=past+r;++t)acc+=scores[t]*historyv[layer][t*ke+(h/(qh/kh))*dim+d];max_error=std::max(max_error,std::abs(acc/den-out[(r*qh+h)*dim+d]));}
        }
      }m4_step_end();
    }m4_finish();need(max_error<1e-5,"paged attention differs from double CPU causal/GQA oracle");
    for(auto p:{qd,kd,vd,od,registered})ck(cudaFree(p));ck(cudaFree(pd));std::ofstream(std::filesystem::path(argv[2])/"oracle.json")<<nlohmann::json({{"status","pass"},{"max_abs_error",max_error},{"limit",1e-5},{"head_dim",dim},{"q_heads",qh},{"kv_heads",kh},{"tokens",18},{"profile_sha256",p.hash}}).dump(2);std::cout<<"paged attention max error "<<max_error<<'\n';return 0;
}catch(const std::exception& e){m4_abort();std::cerr<<e.what()<<'\n';return 1;}
