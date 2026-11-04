#include "hbf_eventlab/runtime/device_driver.hpp"
#include <iostream>
#include <map>
using namespace hbf_eventlab;
// Thin local transaction source: no call to issue/finish/response_delivered and
// no inspection of private Ingress, MOCS, media contents or ReplayState.
class Source {
public:
    explicit Source(DeviceDriver& d):device_(d) {}
    void send(LocalTransaction input,std::function<void(const DeviceResponse&)> callback) {
        const auto id=input.command.id;
        waiting_.emplace(id,Record{std::move(input),std::move(callback)}); offer(id);
    }
    bool drained() const {return waiting_.empty()&&outstanding_.empty();}
private:
    struct Record {LocalTransaction input;std::function<void(const DeviceResponse&)> callback;};
    void offer(CommandId id) {
        auto& record=waiting_.at(id);
        const auto result=device_.try_submit(record.input,[this,id,done=record.callback](const DeliveredResponse& r) {
            if(outstanding_.at(id)!=r.attempt)throw std::logic_error("attempt mismatch");
            outstanding_.erase(id); done(r.response);
        });
        if(result.disposition==HandoffDisposition::Taken) {
            outstanding_.emplace(id,*result.attempt);waiting_.erase(id);
        } else {
            std::vector<WaitObservation> observations;
            for(auto key:result.waits)observations.push_back(device_.notifications().observe(key));
            device_.notifications().subscribe(observations,[this,id]{offer(id);});
        }
    }
    DeviceDriver& device_;
    std::map<CommandId,Record> waiting_;
    std::map<CommandId,TransferAttemptId> outstanding_;
};
int main() {
    try {
        Geometry g{2,2,2,8,4,1};
        StaticDeviceDescription d(g,{256,8,false,true},{256,Tick{10000000},g.block_group_dlus()});
        ExecutionConfiguration c;EventQueue events;events.set_limits({100000,10000,10000});
        DeviceDriver device(d,c,events);Source source(device);
        std::uint64_t serial=0;std::size_t writes=0,reads=0;
        std::cout<<"HBF EventLab autonomous Device demo\n"
                 <<"Tick=1 ps; UCIe=48 GB/s per physical channel per direction; ideal Core Channel\n"
                 <<"RX=16 entries/16384 B; TX=8 entries/512 B; Cache=2 x 4KiB per Bank\n"
                 <<"sense=100000 ps; program=500000 ps; erase=1000000 ps (illustrative)\n";
        device.set_trace([](const DeviceTrace& e) {
            std::cout<<"["<<e.at.value<<" ps] "<<e.action<<" ch="<<e.channel<<" id="<<e.identity;
            if(e.target)std::cout<<" die="<<e.target->die<<" bank="<<e.target->bank<<" page="<<e.target->page;
            std::cout<<"\n";
        });
        for(std::uint32_t bank=0;bank<2;++bank) {
            const LogicalBlockKey b{0,0,bank,0};
            HostCommand w{CommandId{++serial},Op::Write,{0,device.layout().encode_logical({b,0,0})},4096,0};
            source.send({w,std::vector<DataToken>(64,DataToken{100+bank})},[&,b](const DeviceResponse& r) {
                if(std::get<WriteResponse>(r).status!=WriteStatus::Success)throw std::runtime_error("write failed");
                ++writes;
                for(std::uint32_t slot=0;slot<2;++slot) {
                    HostCommand read{CommandId{++serial},Op::Read,{0,device.layout().encode_logical({b,0,slot*64})},
                                     64,0,slot?ReadHint::Batch:ReadHint::Regular};
                    source.send({read,{}},[&,b](const DeviceResponse& result) {
                        const auto& rr=std::get<ReadResponse>(result);
                        if(rr.status!=ReadStatus::Success||rr.data!=DataToken{100+b.bank})
                            throw std::runtime_error("wrong read result");
                        ++reads;
                    });
                }
            });
        }
        events.run();
        if(!source.drained()||!device.activity().drained()||writes!=2||reads!=4)
            throw std::runtime_error("event queue stopped with unfinished work");
        const auto& s=device.statistics();
        std::cout<<"PASS: "<<writes<<" writes, "<<reads<<" reads, "<<s.senses<<" senses, "
                 <<s.cache_hits<<" cache hits, "<<s.delivered<<" exactly-once deliveries\n"
                 <<"Transient state drained; persistent media/cache preserved. Timing is NOT calibrated.\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}

