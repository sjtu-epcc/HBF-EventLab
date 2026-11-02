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
    need(argc==2 || argc==3,"output directory required");const std::string mode=argc==3?argv[2]:"oracle";const bool fault=mode=="fault";std::filesystem::create_directories(argv[1]);
    float *qd,*kd,*vd,*od,*registered;ck(cudaMalloc((void**)&registered,4096));ck(cudaMemset(registered,0,4096));ck(cudaMalloc((void**)&qd,3*2048*4));ck(cudaMalloc((void**)&kd,3*1024*4));ck(cudaMalloc((void**)&vd,3*1024*4));ck(cudaMalloc((void**)&od,3*2048*4));
    int* pd;ck(cudaMalloc((void**)&pd,3*4));
    std::unique_ptr<hbflab::LiveTimingService> service;
    if(fault){hbflab::AsyncRuntimeOptions o;o.backend.geometry=m4_geometry();o.backend.fail_program_after=14337;o.backend.max_events_per_batch=100000000;o.service_timeout_ns=10000000000ull;
        service=std::make_unique<hbflab::LiveTimingService>(o,32,1);service->register_readonly(registered,4096);}
    m4_begin(fault?2:1,service.get(),argv[1]);if(service)service->start();
    std::vector<std::vector<float>> historyk(28),historyv(28);double max_error=0;
    for(int step=0;step<2;++step){const int n=step?1:3,past=step?3:0;m4_step_begin(step);
      for(int layer=0;layer<28;++layer){
        std::vector<float> q(n*2048),k(n*1024),v(k.size()),out(q.size());
        for(unsigned i=0;i<q.size();++i)q[i]=std::sin(float(i+layer*7+step*11)*.013f)*2;
        for(unsigned i=0;i<k.size();++i){k[i]=std::cos(float(i+layer*3+step*13)*.021f);v[i]=std::sin(float(i+layer*5+step*17)*.031f);historyk[layer].push_back(__half2float(__float2half(k[i])));historyv[layer].push_back(__half2float(__float2half(v[i])));}
        ck(cudaMemcpy(qd,q.data(),q.size()*4,cudaMemcpyHostToDevice));ck(cudaMemcpy(kd,k.data(),k.size()*4,cudaMemcpyHostToDevice));ck(cudaMemcpy(vd,v.data(),v.size()*4,cudaMemcpyHostToDevice));
        ggml_tensor pt{},qt{},kt{},vt{},dst{};int positions[]={past,past+1,past+2};if(mode=="position")++positions[0];ck(cudaMemcpy(pd,positions,n*4,cudaMemcpyHostToDevice));pt.type=GGML_TYPE_I32;pt.ne[0]=n;pt.ne[1]=pt.ne[2]=pt.ne[3]=1;pt.data=pd;for(auto t:{&qt,&kt,&vt,&dst}){t->type=GGML_TYPE_F32;t->ne[0]=128;t->ne[1]=t==&qt?16:8;t->ne[2]=n;t->ne[3]=1;t->nb[0]=4;for(int j=1;j<4;++j)t->nb[j]=t->nb[j-1]*t->ne[j-1];}
        qt.data=qd;kt.data=kd;vt.data=vd;dst.data=od;dst.ne[0]=2048;dst.ne[1]=n;dst.ne[2]=1;dst.nb[1]=8192;dst.nb[2]=dst.nb[3]=8192*n;dst.op=GGML_OP_CUSTOM;dst.src[0]=&qt;dst.src[1]=&kt;dst.src[2]=&vt;dst.src[3]=&pt;int tag=0x4d34564b;std::memcpy((char*)dst.op_params+56,&layer,4);std::memcpy((char*)dst.op_params+60,&tag,4);
        if((fault && layer==1) || mode=="position"){bool failed=false;try{m4_attention(&dst,nullptr);}catch(const std::exception& e){const std::string what=e.what();failed=what.find(fault?"shared write terminal failure":"contiguous append positions")!=std::string::npos;}
          need(failed,"write failure was accepted");need(!std::filesystem::exists(std::filesystem::path(argv[1])/"kv-summary.json"),"failed KV published summary");m4_abort();service.reset();for(auto p:{qd,kd,vd,od,registered})ck(cudaFree(p));ck(cudaFree(pd));std::cout<<mode<<" rejected as expected\n";return 0;}
        m4_attention(&dst,nullptr);ck(cudaMemcpy(out.data(),od,out.size()*4,cudaMemcpyDeviceToHost));
        for(int r=0;r<n;++r)for(int h=0;h<16;++h){std::vector<double> scores(past+r+1);double mx=-1e300,den=0;
          for(int t=0;t<=past+r;++t){double dot=0;for(int d=0;d<128;++d)dot+=double(q[(r*16+h)*128+d])*historyk[layer][t*1024+(h/2)*128+d];scores[t]=dot/std::sqrt(128.0);mx=std::max(mx,scores[t]);}
          for(auto& a:scores){a=std::exp(a-mx);den+=a;}
          for(int d=0;d<128;++d){double acc=0;for(int t=0;t<=past+r;++t)acc+=scores[t]*historyv[layer][t*1024+(h/2)*128+d];max_error=std::max(max_error,std::abs(acc/den-out[(r*16+h)*128+d]));}
        }
      }m4_step_end();
    }m4_finish();need(max_error<1e-5,"paged attention differs from double CPU causal/GQA oracle");
    for(auto p:{qd,kd,vd,od,registered})ck(cudaFree(p));ck(cudaFree(pd));std::cout<<"56 layer calls, 4 tokens, paged attention max error "<<max_error<<'\n';return 0;
}catch(const std::exception& e){m4_abort();std::cerr<<e.what()<<'\n';return 1;}
