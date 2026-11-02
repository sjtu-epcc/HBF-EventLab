#include "capacity_capacity_probe.hpp"
#include "hbflab/capacity_gpu.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
using namespace hbflab;
using json=nlohmann::ordered_json;
namespace {
void ck(cudaError_t e){if(e!=cudaSuccess)throw std::runtime_error(cudaGetErrorString(e));}
__global__ void probe_write(unsigned* page,unsigned step){page[threadIdx.x]=10000+step*64+threadIdx.x;}
CapacityOptions options(){CapacityOptions o;o.logical_pages=2;o.frames=1;return o;}
}
struct M3CapacityProbe::Impl {
    LiveTimingService& service;CapacityGpu gpu;
    Allocation allocation;cudaStream_t stream=nullptr;
    std::filesystem::path output;std::ofstream audit;
    unsigned count=0;std::uint64_t records=0;
    Impl(LiveTimingService& s,const std::string& path):service(s),gpu(options(),&s),output(path) {
        allocation=gpu.store().allocate(8192);ck(cudaStreamCreateWithFlags(&stream,cudaStreamNonBlocking));
        audit.open(output/"hbf-media.jsonl");if(!audit)throw std::runtime_error("media audit open failed");
    }
    ~Impl(){if(stream){cudaStreamSynchronize(stream);cudaStreamDestroy(stream);}}
    void drain() {
        for(const auto& t:service.take_media_audit()) {
            audit<<json({{"ticket",t.ticket},{"dispatch",t.dispatch},{"producer",t.producer},{"write",t.write},
                {"logical",t.logical},{"bytes",t.bytes},{"floor_ns",t.floor_ns},{"arrival_ns",t.arrival_ns},
                {"arrival_ps",t.arrival_ps},{"end_ps",t.end_ps},{"replayed_pages",t.replayed_pages},
                {"ready_wall_ns",t.ready_wall_ns},{"completion_wall_ns",t.completion_wall_ns},
                {"resumed_wall_ns",t.resumed_wall_ns},{"delay_ns",t.delay_ns}}).dump()<<'\n';++records;
        }
        if(!audit)throw std::runtime_error("media audit write failed");
    }
    void check(GpuPageLease lease,unsigned step,bool initial) {
        unsigned actual[32];ck(cudaMemcpyAsync(actual,gpu.data(lease),sizeof(actual),cudaMemcpyDeviceToHost,stream));ck(cudaStreamSynchronize(stream));
        for(unsigned i=0;i<32;++i)if(actual[i]!=(initial?0:10000+step*64+i))throw std::runtime_error("M3 page bytes mismatch");
    }
};
M3CapacityProbe::M3CapacityProbe(LiveTimingService& s,const std::string& output):impl_(std::make_unique<Impl>(s,output)){}
M3CapacityProbe::~M3CapacityProbe()=default;
void M3CapacityProbe::step(unsigned step) {
    auto& s=*impl_;if(step!=s.count)throw std::runtime_error("M3 capacity step order mismatch");
    auto lease=s.gpu.acquire(s.allocation,step%2,PageAccess::Write,s.stream);
    s.check(lease,step>=2?step-2:0,step<2);
    probe_write<<<1,32,0,s.stream>>>(static_cast<unsigned*>(s.gpu.writable_data(lease)),step);ck(cudaGetLastError());s.gpu.finish(lease);
    auto hit=s.gpu.acquire(s.allocation,step%2,PageAccess::Read,s.stream);s.check(hit,step,false);s.gpu.finish(hit);
    ++s.count;s.drain();
}
void M3CapacityProbe::finish() {
    auto& s=*impl_;s.gpu.flush();s.drain();s.audit.close();if(!s.audit)throw std::runtime_error("media audit close failed");
    const auto c=s.gpu.store().statistics();const auto g=s.gpu.statistics();
    if(c.faults!=s.count || c.hits!=s.count || c.writebacks!=s.count || c.dirty_frames || c.pins || g.active_leases || s.records!=2*s.count)
        throw std::runtime_error("M3 capacity accounting mismatch");
    std::ofstream out(s.output/"hbf-capacity.json");
    out<<json({{"scope","synthetic independent capacity workload; not model KV"},{"steps",s.count},
        {"faults",c.faults},{"hits",c.hits},{"evictions",c.evictions},{"writebacks",c.writebacks},
        {"dirty_frames",c.dirty_frames},{"pins",c.pins},{"media_records",s.records},{"device_bytes",g.device_bytes},
        {"uploads",g.uploads},{"downloads",g.downloads},{"h2d_bytes",g.h2d_bytes},{"d2h_bytes",g.d2h_bytes}}).dump(2)<<'\n';
    out.close();if(!out)throw std::runtime_error("M3 capacity report failed");s.gpu.store().release(s.allocation);
}
