#include "hbf_eventlab/runtime/device_driver.hpp"
#include <iostream>
#include <string>
#include <set>
#include <map>
using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool b,const char* why) {++checks;if(!b) throw std::runtime_error(why);}
template<class E=std::exception,class F> void rejects(F f) {
    bool caught=false;try{f();}catch(const E&){caught=true;}check(caught,"expected diagnostic");
}
StaticDeviceDescription profile(std::uint32_t mocs=256,Tick timeout=Tick{10000000}) {
    Geometry g{2,2,2,8,4,1};
    return {g,{256,8,false,true},{mocs,timeout,g.block_group_dlus()}};
}
struct Logged { std::string action; Tick at; ChannelId channel; std::uint64_t identity; std::optional<PhysicalNandAddress> target; };
struct Fixture {
    EventQueue events;
    DeviceDriver device;
    std::uint64_t serial=0;
    std::map<CommandId,LocalTransaction> source;
    std::map<CommandId,WaitId> waits;
    std::map<CommandId,DeliveredResponse> replies;
    std::map<CommandId,TransferAttemptId> taken;
    std::vector<CommandId> delivery_order;
    std::vector<Logged> trace;
    explicit Fixture(ExecutionConfiguration c={},std::uint32_t mocs=256,Tick timeout=Tick{10000000},
                     std::unique_ptr<INandSchedulingPolicy> nand=std::make_unique<FcfsNandPolicy>(),
                     std::unique_ptr<IUcieArbitrationPolicy> link=std::make_unique<FifoUciePolicy>())
        :device(profile(mocs,timeout),c,events,std::move(nand),std::move(link)) {
        events.set_limits({100000,10000,10000});
        device.set_trace([this](const auto& e){trace.push_back({e.action,e.at,e.channel,e.identity,e.target});});
    }
    LocalTransaction tx(Op op,LogicalBlockKey b={0,0,0,0},PageId page=0,std::uint32_t slot=0,
                        std::uint32_t bytes=64,std::uint64_t token=17,ReadHint hint=ReadHint::Regular,
                        std::optional<ReadGroup> group={}) {
        HostCommand cmd{CommandId{++serial},op,{b.channel,device.layout().encode_logical({b,page,slot*64})},bytes,0,hint,group};
        return {cmd,op==Op::Write?std::vector<DataToken>(bytes/64,DataToken{token}):std::vector<DataToken>{}};
    }
    void offer(CommandId id) {
        const auto result=device.try_submit(source.at(id),[this,id](const DeliveredResponse& r) {
            check(taken.at(id)==r.attempt,"handoff identity preserved");
            check(!replies.count(id),"exactly once final response");
            replies.emplace(id,r);delivery_order.push_back(id);taken.erase(id);
        });
        if(result.disposition==HandoffDisposition::Taken) {
            taken.emplace(id,*result.attempt);source.erase(id);
        } else {
            check(!result.waits.empty(),"NotTaken has wake target");
            std::vector<WaitObservation> observations;
            for(auto key:result.waits) observations.push_back(device.notifications().observe(key));
            waits[id]=device.notifications().subscribe(observations,[this,id]{waits.erase(id);offer(id);});
        }
    }
    CommandId send(LocalTransaction t) {const auto id=t.command.id;source.emplace(id,std::move(t));offer(id);return id;}
    void run() {
        events.run();check(source.empty()&&taken.empty()&&waits.empty(),"source ownership drained");
        check(device.activity().drained(),"temporary Device activity drained");
    }
    CommandId write(LogicalBlockKey b={0,0,0,0},PageId page=0,std::uint64_t token=17) {
        return send(tx(Op::Write,b,page,0,4096,token));
    }
    void warm(LogicalBlockKey b={0,0,0,0},PageId count=1) {
        for(PageId page=0;page<count;++page) {const auto id=write(b,page,100+page);run();check(wr(id).status==WriteStatus::Success,"warm write");}
    }
    const ReadResponse& rr(CommandId id) const {return std::get<ReadResponse>(replies.at(id).response);}
    const WriteResponse& wr(CommandId id) const {return std::get<WriteResponse>(replies.at(id).response);}
    Tick moment(const char* action,std::uint64_t id=0) const {
        for(const auto& e:trace) if(e.action==action && (!id || e.identity==id)) return e.at;
        throw std::runtime_error(std::string("missing trace: ")+action);
    }
    std::size_t count(const char* action) const {std::size_t n=0;for(const auto&e:trace)n+=e.action==action;return n;}
};
void auto_write_read() {
    Fixture f; const auto w=f.write();f.run();check(f.wr(w).status==WriteStatus::Success,"automatic write");
    const auto r=f.send(f.tx(Op::Read));f.run();check(f.rr(r).data==DataToken{17},"automatic NAND read");
    check(f.device.statistics().programs==1&&f.device.statistics().erases==1&&f.device.statistics().senses==1,"all media driven by Device");
    check(f.device.cache().contains({0,0,0,0,0,0}),"persistent cache survives drain");
}
void split_write() {
    Fixture f;
    for(std::uint32_t slot=0;slot<64;++slot) f.send(f.tx(Op::Write,{0,0,0,0},0,slot,64,slot+9));
    f.run();check(f.replies.size()==64&&f.device.statistics().programs==1,"64 commands aggregate one program");
    const auto id=f.send(f.tx(Op::Read,{0,0,0,0},0,31));f.run();check(f.rr(id).data==DataToken{40},"fragment data preserved");
}
void bank_parallel() {
    Fixture f;f.warm();f.warm({0,0,1,0});f.trace.clear();
    f.send(f.tx(Op::Read,{0,0,0,0},0,0,64,0,ReadHint::Batch));
    f.send(f.tx(Op::Read,{0,0,1,0},0,0,64,0,ReadHint::Batch));f.run();
    std::vector<Tick> start,end;for(auto&e:f.trace) {
        if(e.action=="sense-issue")start.push_back(e.at);if(e.action=="sense-finish")end.push_back(e.at);
    }
    check(start.size()==2&&end.size()==2&&start[1]<end[0],"different Banks on SAME die overlap");
}
void bank_serial() {
    Fixture f;f.warm({},3);f.trace.clear();
    for(PageId p=0;p<3;++p)f.send(f.tx(Op::Read,{},p,0,64,0,ReadHint::Batch));
    f.run();std::optional<Tick> finish;bool active=false;
    for(auto&e:f.trace) {
        if(e.action=="sense-issue"){check(!active,"same Bank not double occupied");active=true;if(finish)check(!(e.at<*finish),"serial media order");}
        if(e.action=="sense-finish"){active=false;finish=e.at;}
    }
    check(f.device.statistics().senses==3,"three distinct pages sensed");
}
void mocs_ingress() {
    Fixture f({},1);const auto w=f.write();const auto r=f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch));
    while(f.device.statistics().accepted<1 || f.device.activity().ingress<1) check(f.events.run_next(),"events until queued RX");
    check(f.device.activity().outstanding==1&&f.device.statistics().accepted==1,"RX resident does not occupy MOCS");
    f.run();check(f.wr(w).status==WriteStatus::Success&&f.rr(r).data==DataToken{17},"credit release retries RX");
    check(f.device.statistics().request_transfers==2,"credit wait does not retransmit link");
}
void rx_reservation() {
    ExecutionConfiguration c;c.ingress={1,4096};c.request_fixed_latency=Tick{50000};Fixture f(c);
    auto a=f.tx(Op::Write);auto b=f.tx(Op::Read);
    const auto first=f.device.try_submit(a,[](auto&){});check(first.disposition==HandoffDisposition::Taken,"first handoff");
    const auto second=f.device.try_submit(b,[](auto&){});check(second.disposition==HandoffDisposition::NotTaken,"RX no oversell");
    check(f.device.ingress(0).entries()==1&&f.device.ingress(0).bytes()==64,"reservation charged before arrival");
    check(f.device.activity().outstanding==0&&f.device.writes().pending(0)==0,"in-flight not formally accepted");
    check(f.device.activity().in_flight_requests==1,"in-flight counted");
}
void window_batch_timer() {
    ExecutionConfiguration c;c.read_window=Tick{50000};c.read_threshold=0;Fixture f(c);
    for(auto hint:{ReadHint::Regular,ReadHint::Regular,ReadHint::Batch,ReadHint::Batch,ReadHint::Regular})
        f.send(f.tx(Op::Read,{},0,0,64,0,hint));
    f.run();check(f.device.statistics().window_batches==3&&f.count("window-release")==5,"RRBBR -> RRB/B/R timer");
    check(!(f.moment("sense-issue")<f.moment("window-release")),"sense waits release");
}
void window_threshold() {
    ExecutionConfiguration c;c.read_window=Tick{50000};c.read_threshold=2;Fixture f(c);
    f.send(f.tx(Op::Read));f.send(f.tx(Op::Read));f.run();
    check(f.device.statistics().window_batches==1,"threshold one batch");
    check(f.moment("sense-issue").value<50000,"threshold avoids timer wait");
}
void window_channel_isolation() {
    ExecutionConfiguration c;c.read_window=Tick{50000};c.read_threshold=0;Fixture f(c);
    const auto a=f.send(f.tx(Op::Read));
    f.send(f.tx(Op::Read,{1,0,0,0},0,0,64,0,ReadHint::Batch));f.run();
    check(f.moment("window-release",a.value).value>=50000,"other channel Batch cannot close window");
}
void batch_pending() {
    ExecutionConfiguration c;c.read_window=Tick{50000};c.read_threshold=0;Fixture f(c);
    f.send(f.tx(Op::Write,{0,0,1,0},0,0));
    f.send(f.tx(Op::Read,{0,0,0,0}));
    const auto b=f.send(f.tx(Op::Read,{0,0,1,0},0,0,64,0,ReadHint::Batch));f.run();
    check(f.rr(b).data==DataToken{17},"Pending B forwards");
    check(f.device.statistics().senses==1,"Pending does not sense");
    check(f.moment("window-release").value<50000,"forwarded B releases earlier Regular");
}
void batch_missing() {
    ExecutionConfiguration c;c.read_window=Tick{50000};c.read_threshold=0;Fixture f(c);
    f.send(f.tx(Op::Write,{0,0,1,0},0,0));
    f.send(f.tx(Op::Read));
    const auto b=f.send(f.tx(Op::Read,{0,0,1,0},0,1,64,0,ReadHint::Batch));f.run();
    check(f.rr(b).status==ReadStatus::PendingWriteMissing&&f.device.statistics().senses==1,"0xA no NAND");
    check(f.moment("window-release").value<50000,"missing B still closes");
}
void cache_hit() {
    Fixture f;f.warm();auto a=f.send(f.tx(Op::Read));f.run();f.trace.clear();
    auto b=f.send(f.tx(Op::Read,{},0,1));f.run();
    check(f.rr(a).data==f.rr(b).data&&f.device.statistics().senses==1&&f.device.statistics().cache_hits==1,"hit without sense");
    check(f.count("window-release")==0,"early hit bypasses window");
}
void miss_becomes_hit() {
    Fixture f;f.warm();
    for(std::uint32_t i=0;i<8;++i)f.send(f.tx(Op::Read,{},0,i,64,0,ReadHint::Batch));
    f.run();check(f.device.statistics().senses==1&&f.device.statistics().cache_hits==7,"waiting misses become hits");
    check(f.replies.size()==9,"all reads retained");
}
void ordered_pages() {
    Fixture f;f.warm({},2);
    f.send(f.tx(Op::Read,{},1,0,64,0,ReadHint::Batch));f.run();f.trace.clear();f.delivery_order.clear();
    const auto a=f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch,7));
    const auto b=f.send(f.tx(Op::Read,{},1,0,64,0,ReadHint::Batch,7));
    const auto c=f.send(f.tx(Op::Read,{},1,1,64,0,ReadHint::Batch,8));f.run();
    check(f.replies.at(a).at.value<=f.replies.at(b).at.value,"same group distinct pages ordered");
    check(f.replies.at(c).at<f.replies.at(a).at,"independent group cache hit bypasses blocked group");
    check(f.rr(a).data==DataToken{100}&&f.rr(b).data==DataToken{101},"correct distinct-page results");
}
void ordered_error() {
    Fixture f;f.warm();
    auto bad=f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch,5);
    bad.command.address.local=LocalByteAddress{f.device.description().geometry().channel_bytes()};
    const auto a=f.send(bad);const auto b=f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch,5));f.run();
    check(f.rr(a).status==ReadStatus::InvalidAddress&&f.rr(b).status==ReadStatus::Success,"error advances order head");
    check(f.replies.at(a).at.value<=f.replies.at(b).at.value,"error ordering preserved");
}
void pending_order_diagnostic() {
    Fixture f;f.send(f.tx(Op::Write));f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch,3));
    rejects<std::invalid_argument>([&]{f.events.run();});
    check(f.device.statistics().accepted==1&&f.device.activity().ingress==1,"unsupported ordered Pending stays unsettled before MOCS");
    check(f.replies.empty(),"no invented error completion");
}
void tx_cache_pressure() {
    ExecutionConfiguration c;c.egress={1,64};c.response_fixed_latency=Tick{2000000};Fixture f(c);
    f.warm({},4);f.warm({0,0,1,0});f.trace.clear();
    for(PageId p=0;p<4;++p)f.send(f.tx(Op::Read,{},p,0,64,0,ReadHint::Batch));
    f.send(f.tx(Op::Read,{0,0,1,0},0,0,64,0,ReadHint::Batch));
    std::size_t max_protected=0;
    while(f.events.run_next()) {
        max_protected=std::max(max_protected,f.device.cache().protected_slots());
        check(f.device.egress(0).entries()<=1&&f.device.egress(0).bytes()<=64,"finite TX budget");
    }
    check(f.device.activity().drained(),"pressure eventually drains");
    check(max_protected>=2&&f.device.statistics().cache_blocked>0,"two protected slots stall third miss");
    Tick page3{},other{},first_delivery=f.moment("delivered");
    for(auto&e:f.trace)if(e.action=="sense-issue") {
        if(e.target->bank==0&&e.target->page==3)page3=e.at;
        if(e.target->bank==1)other=e.at;
    }
    check(!(page3<first_delivery)&&other<first_delivery,"cache backpressure Bank-local, independent Bank proceeds");
}
void cache_rewrite() {
    Fixture f;f.warm();const auto old=f.send(f.tx(Op::Read));f.run();
    const auto w=f.write({},0,900);f.run();const auto r=f.send(f.tx(Op::Read));f.run();
    check(f.rr(old).data==DataToken{100}&&f.rr(r).data==DataToken{900},"rewrite invalidates same physical tag");
    check(f.wr(w).status==WriteStatus::Success&&f.device.statistics().senses==2,"rewrite new sense");
}
void pending_frozen() {
    ExecutionConfiguration c;c.response_fixed_latency=Tick{500000};Fixture f(c);
    f.send(f.tx(Op::Write));const auto a=f.send(f.tx(Op::Read));const auto b=f.send(f.tx(Op::Read,{},0,1));
    f.send(f.tx(Op::Write,{},0,1,64,99));f.run();
    check(f.rr(a).data==DataToken{17}&&f.rr(b).status==ReadStatus::PendingWriteMissing,"accepted Pending observation frozen");
    check(f.device.statistics().senses==0,"Pending reads bypass NAND");
}
void pending_tx_full() {
    ExecutionConfiguration c;c.egress={1,64};c.response_fixed_latency=Tick{500000};Fixture f(c);
    f.send(f.tx(Op::Write));const auto a=f.send(f.tx(Op::Read));
    const auto b=f.send(f.tx(Op::Read));f.run();
    check(f.rr(a).data==DataToken{17}&&f.rr(b).data==DataToken{17},"Pending waits finite result reservation");
    check(f.device.statistics().request_transfers==3,"waiting Pending not retransmitted");
    check(f.moment("accepted",b.value).value>=f.replies.at(a).at.value,"no Pending acceptance before result space");
}
void rejected_batch_no_effect() {
    ExecutionConfiguration c;c.read_window=Tick{50000};c.read_threshold=0;Fixture f(c,1);
    const auto a=f.send(f.tx(Op::Read));
    const auto b=f.send(f.tx(Op::Read,{},0,1,64,0,ReadHint::Batch));f.run();
    check(f.moment("window-release",a.value).value>=50000,"unaccepted B cannot flush A");
    check(f.moment("accepted",b.value).value>=f.replies.at(a).at.value,"B waits MOCS");
}
void ingress_bypass() {
    Fixture f;f.warm();
    const auto partial=f.send(f.tx(Op::Write,{},1,0));
    const auto rewrite=f.send(f.tx(Op::Write,{},0,0,4096,80));
    for(std::uint32_t slot=1;slot<64;++slot)f.send(f.tx(Op::Write,{},1,slot));
    f.run();check(f.wr(partial).status==WriteStatus::Success&&f.wr(rewrite).status==WriteStatus::Success,"old DLU supplements bypass blocked page0 RX");
    check(f.device.statistics().programs==3,"no timeout-induced fake progress");
}
void fault_fanout() {
    Fixture f;f.warm();f.device.set_program_outcome([](const auto& op){return op.physical.page==1?ProgramOutcome::FailedReusable:ProgramOutcome::Success;});
    const auto a=f.write({},1);const auto b=f.write({},2);f.run();
    check(f.wr(a).status==WriteStatus::ProgramFail&&f.wr(b).status==WriteStatus::WriteOrderViolation&&f.wr(b).additional_info==2,"failure followers fanout");
    const auto r=f.send(f.tx(Op::Read));f.run();check(f.rr(r).status==ReadStatus::TemporarilyRestricted,"read recovery restriction");
}
void replacement_replay() {
    Fixture f;f.device.set_program_outcome([](const auto&){return ProgramOutcome::FailedUnusable;});
    auto a=f.write();f.run();check(f.wr(a).status==WriteStatus::ProgramFail,"injected failure");
    f.device.set_program_outcome([](const auto&){return ProgramOutcome::Success;});
    auto replay=f.write({},0,55);f.run();auto r=f.send(f.tx(Op::Read));f.run();
    check(f.wr(replay).status==WriteStatus::Success&&f.rr(r).data==DataToken{55},"explicit replay through autonomous Device");
    bool spare=false;for(auto&e:f.trace)if(e.action=="program-issue"&&e.target->block>=4)spare=true;
    check(spare,"same-Bank spare used"); // Host recovery policy is stage 3.
}
void remap_cache() {
    Fixture f;f.warm();f.send(f.tx(Op::Read));f.run();check(f.device.cache().contains({0,0,0,0,0,0}),"cache initially valid");
    f.device.declare_discardable(0,0);f.device.declare_discardable(0,1);
    check(f.device.quiesce_pair(0,0,1),"quiesce idle zones");
    check(f.device.commit_swap(0,0,1)==RemapResult::Applied,"swap commit");
    check(!f.device.cache().contains({0,0,0,0,0,0}),"remap invalidates tags");
    auto r=f.send(f.tx(Op::Read));f.run();check(f.rr(r).status==ReadStatus::UnwrittenPage,"discarded logical contents not exposed");
}
void duplicate_handoff() {
    Fixture f;auto cmd=f.tx(Op::Read);f.send(cmd);
    rejects<std::logic_error>([&]{f.device.try_submit(cmd,[](auto&){});});
    f.run();check(f.replies.size()==1,"duplicate has no second response");
    bool delivered=false;
    auto result=f.device.try_submit(cmd,[&](auto&){delivered=true;});check(result.disposition==HandoffDisposition::Taken,"ID can be reused after delivery");
    f.events.run();check(delivered&&f.device.activity().drained(),"new attempt settles");
}
void response_feedback() {
    Fixture f;auto t=f.tx(Op::Write,{},0,0,4096);bool callback=false;
    auto result=f.device.try_submit(t,[&](const auto&) {
        callback=true;check(f.device.activity().outstanding==0&&f.device.egress(0).entries()==0,"callback after credit/storage release");
        f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch));
    });
    check(result.disposition==HandoffDisposition::Taken&&!callback,"no synchronous callback");
    f.run();check(callback&&f.replies.size()==1,"callback can safely submit next request");
}
void configuration() {
    auto d=profile();ExecutionConfiguration c;
    check(c.serialization(64)==Tick{1334},"48 GB/s counted in ps with ceiling");
    c.effective_gbytes_per_second=94;check(c.serialization(64)==Tick{681},"94 GB/s effective rate");
    c.effective_gbytes_per_second=188;check(c.serialization(64)==Tick{341},"188 GB/s effective rate");
    c.core_channels_per_ucie=2;c.validate(d);CoreChannelTopology topology(d.geometry(),c);
    check(topology.core_channel(0)==0&&topology.core_channel(1)==1&&d.geometry().dies_per_channel==2,"Core grouping not extra capacity");
    c.ingress.payload_bytes=64;rejects<std::invalid_argument>([&]{c.validate(d);});
    c.ingress.payload_bytes=4096;c.effective_gbytes_per_second=0;rejects<std::invalid_argument>([&]{c.validate(d);});
}
void cache_pin_invalidation() {
    BankCache cache;PageData a{},b{},c{};a.fill(DataToken{1});b.fill(DataToken{2});c.fill(DataToken{3});
    auto f1=cache.begin_fill({0,0,0,0,0,0},&a);cache.finish_fill(*f1);auto r1=cache.pin({0,0,0,0,0,0});
    auto f2=cache.begin_fill({0,0,0,0,1,0},&b);cache.finish_fill(*f2);auto r2=cache.pin({0,0,0,0,1,0});
    check(!cache.can_fill({0,0,0}),"both protected slots cannot evict");
    cache.invalidate({0,0,0,0});check(!cache.contains({0,0,0,0,0,0})&&r1->load()==DataToken{1},"invalid tag does not destroy response");
    r1.reset();auto f3=cache.begin_fill({0,0,0,0,2,0},&c);check(f3.has_value(),"released invalid slot reusable");
    cache.finish_fill(*f3);check(r2->load()==DataToken{2},"other saved response unchanged");
    rejects<std::logic_error>([&]{cache.finish_fill(*f1);});
}
void teardown() {
    EventQueue q;{
        auto d=std::make_unique<DeviceDriver>(profile(),ExecutionConfiguration{},q);
        LocalTransaction t{{CommandId{1},Op::Read,{0,LocalByteAddress{0}},64,0,ReadHint::Batch},{}};
        d->try_submit(t,[](auto&){throw std::runtime_error("destroyed Driver callback");});
        while(d->statistics().senses==0)check(q.run_next(),"reach active sense");
    }
    q.run();check(q.pending()==0,"destroy cancels events without dangling cache handles");
}
void unsupported() {
    Fixture f;auto t=f.tx(Op::Read);t.command.size_bytes=4096;
    rejects<std::invalid_argument>([&]{f.device.try_submit(t,[](auto&){});});
    check(f.device.activity().drained()&&f.events.pending()==0,"unsupported request not taken");
}
class ReverseLink final:public IUcieArbitrationPolicy {
public:std::uint64_t select(const std::vector<TransferCandidateView>& c)const override{return c.back().identity;}
};
class ReadFirst final:public INandSchedulingPolicy {
public:std::uint64_t select(const std::vector<MediaCandidateView>& c)const override {
    for(auto&x:c)if(x.kind==MediaOperationKind::Read)return x.identity;return c.front().identity;
}};
void policies() {
    Fixture f({},256,Tick{10000000},std::make_unique<ReadFirst>(),std::make_unique<ReverseLink>());
    f.warm();f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch));
    f.send(f.tx(Op::Read,{0,0,1,0},0,0,64,0,ReadHint::Batch));f.run();
    check(f.device.statistics().senses==2,"separate injectable policies preserve legality");
}
class InvalidNand final:public INandSchedulingPolicy {
public:std::uint64_t select(const std::vector<MediaCandidateView>&)const override{return 999999;}
};
void invalid_policy() {
    Fixture f({},256,Tick{10000000},std::make_unique<InvalidNand>());
    f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch));rejects<std::logic_error>([&]{f.events.run();});
    check(f.device.resources().active()==0&&f.device.activity().outstanding==1,"invalid policy cannot commit resources");
}
void link_latency() {
    ExecutionConfiguration c;c.request_fixed_latency=Tick{2000};c.response_fixed_latency=Tick{3000};
    Fixture f(c);auto cmd=f.tx(Op::Read);cmd.command.address.local=LocalByteAddress{f.device.description().geometry().channel_bytes()};
    auto id=f.send(cmd);f.run();
    const auto req=c.serialization(c.request_control_bytes).value+c.request_fixed_latency.value;
    const auto resp=c.serialization(c.response_control_bytes).value+c.response_fixed_latency.value;
    check(f.moment("accepted").value==req&&f.replies.at(id).at.value==req+resp,"service plus fixed delay modeled independently");
}
void pending_recheck() {
    ExecutionConfiguration c;c.egress={1,64};c.response_fixed_latency=Tick{20000000};Fixture f(c);
    f.send(f.tx(Op::Write));auto a=f.send(f.tx(Op::Read));auto b=f.send(f.tx(Op::Read));f.run();
    check(f.rr(a).data==DataToken{17},"accepted forward survives assembly timeout");
    check(f.rr(b).status==ReadStatus::UnwrittenPage,"unaccepted Pending read rechecks after timeout, cannot keep old observation");
}
void deadline_link_boundary() {
    auto run=[](bool late) {
        ExecutionConfiguration c;c.request_fixed_latency=Tick{0};c.program=Tick{1000};c.erase=Tick{1000};
        Fixture f(c,256,Tick{200000});
        const auto first=f.send(f.tx(Op::Write));
        for(std::uint32_t slot=1;slot<63;++slot)f.send(f.tx(Op::Write,{},0,slot));
        const auto arrival=c.serialization(80).value+200000+(late?1:0);
        const auto start=arrival-c.serialization(80).value;
        f.events.schedule_at(Tick{start},EventPhase::Arrival,[&]{f.send(f.tx(Op::Write,{},0,63));});
        f.run();return f.wr(first).status;
    };
    check(run(false)==WriteStatus::Success,"last fragment formally accepted AT deadline rescues assembly");
    check(run(true)==WriteStatus::AssemblyTimeout,"positive link delay AFTER deadline cannot rescue");
}
class ReverseNand final:public INandSchedulingPolicy {
public:std::uint64_t select(const std::vector<MediaCandidateView>& c)const override{return c.back().identity;}
};
void sense_order_policy() {
    ExecutionConfiguration c;c.read_window=Tick{50000};c.read_threshold=3;
    Fixture f(c,256,Tick{10000000},std::make_unique<ReverseNand>());
    f.warm({},3);f.trace.clear();
    for(PageId p=0;p<3;++p)f.send(f.tx(Op::Read,{},p));
    f.run();std::vector<PageId> order;
    for(auto&e:f.trace)if(e.action=="sense-issue")order.push_back(e.target->page);
    check(order==std::vector<PageId>({0,1,2}),"Bank sense order is legality, not replaceable priority");
}
void sense_order_blocked() {
    Fixture f;f.warm({},2);f.warm({0,0,1,0});f.trace.clear();
    f.send(f.tx(Op::Read,{0,0,1,0},0,0,64,0,ReadHint::Batch,7));
    f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch,7));
    f.send(f.tx(Op::Read,{},1,0,64,0,ReadHint::Batch,8));f.run();
    std::vector<PageId> order;
    for(auto&e:f.trace)if(e.action=="sense-issue"&&e.target->bank==0)order.push_back(e.target->page);
    check(order==std::vector<PageId>({0,1}),"younger same-Bank sense cannot bypass older order-blocked read");
}
void ordered_late_error() {
    ExecutionConfiguration c;c.egress={1,64};Fixture f(c);f.warm();
    auto a=f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch,9));
    auto bad=f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch,9);
    bad.command.address.local=LocalByteAddress{f.device.description().geometry().channel_bytes()};
    auto b=f.send(bad);f.run();
    check(f.rr(a).status==ReadStatus::Success&&f.rr(b).status==ReadStatus::InvalidAddress,"later error cannot occupy all TX before group head");
    check(f.replies.at(a).at<f.replies.at(b).at,"group head delivered first");
}
void rewrite_saved_read() {
    ExecutionConfiguration c;c.egress={1,64};c.response_fixed_latency=Tick{3000000};Fixture f(c);f.warm();f.trace.clear();
    auto r=f.send(f.tx(Op::Read));auto w=f.write({},0,900);f.run();
    check(f.rr(r).data==DataToken{100}&&f.wr(w).status==WriteStatus::Success,"read old contents survive actual rewrite");
    check(!(f.moment("accepted",w.value)<f.moment("sense-finish")),"rewrite waits for logical read result");
    check(f.moment("accepted",w.value)<f.replies.at(r).at,"saved read releases content before link delivery");
}
void retirement_loop() {
    Fixture f;f.device.set_program_outcome([](const auto&){return ProgramOutcome::FailedUnusable;});
    auto a=f.write();f.run();auto b=f.write();f.run();auto c=f.write();f.run();
    check(f.wr(a).status==WriteStatus::ProgramFail&&f.wr(b).status==WriteStatus::ProgramFail,"original and spare fail");
    check(f.wr(c).status==WriteStatus::CapacityUnusable,"no spare capacity response");
    auto r=f.send(f.tx(Op::Read));f.run();check(f.rr(r).status==ReadStatus::InvalidAddress,"retired logical capacity cannot read cached bytes");
}
void teardown_cache_hit() {
    EventQueue q;
    {
        DeviceDriver d(profile(),ExecutionConfiguration{},q);
        LocalTransaction w{{CommandId{1},Op::Write,{0,LocalByteAddress{0}},4096,0},std::vector<DataToken>(64,DataToken{99})};
        d.try_submit(w,[](auto&){});q.run();
        LocalTransaction r{{CommandId{2},Op::Read,{0,LocalByteAddress{0}},64,0,ReadHint::Batch},{}};
        d.try_submit(r,[](auto&){});q.run();r.command.id=CommandId{3};
        d.try_submit(r,[](auto&){throw std::runtime_error("canceled hit delivered");});
        while(d.statistics().cache_hits==0)check(q.run_next(),"reach pinned cache hit");
        check(d.cache().protected_slots()==1,"driver owns a protected hit awaiting cache service");
    }
    q.run();check(q.pending()==0,"canceled event closure never owns dangling Cache lease");
}
void callback_limit() {
    Fixture f;auto r=f.send(f.tx(Op::Read,{},0,0,64,0,ReadHint::Batch));
    f.events.set_limits({2,10000,10000});
    rejects<EventLimitReached>([&]{f.events.run();});
    check(f.replies.empty()&&!f.device.activity().drained(),"limit preserves unsettled transfer");
    f.events.set_limits({100000,10000,10000});f.run();check(f.rr(r).status==ReadStatus::UnwrittenPage,"can resume without duplicate handoff");
}
void multi_channel_bandwidth() {
    Fixture f;auto a=f.tx(Op::Read);auto b=f.tx(Op::Read,{1,0,0,0});
    a.command.address.local=LocalByteAddress{f.device.description().geometry().channel_bytes()};
    b.command.address.local=a.command.address.local;
    auto id1=f.send(a);auto id2=f.send(b);f.run();
    check(f.replies.at(id1).at==f.replies.at(id2).at,"independent physical channels overlap at full configured rate");
}


}
int main(int argc,char**argv) {
    const std::map<std::string,void(*)()> cases{
        {"sense_order_policy",sense_order_policy},{"sense_order_blocked",sense_order_blocked},
        {"ordered_late_error",ordered_late_error},{"rewrite_saved_read",rewrite_saved_read},
        {"retirement_loop",retirement_loop},{"teardown_cache_hit",teardown_cache_hit},
        {"callback_limit",callback_limit},{"multi_channel_bandwidth",multi_channel_bandwidth},
        {"auto_write_read",auto_write_read},{"split_write",split_write},{"bank_parallel",bank_parallel},{"bank_serial",bank_serial},
        {"mocs_ingress",mocs_ingress},{"rx_reservation",rx_reservation},{"window_batch_timer",window_batch_timer},
        {"window_threshold",window_threshold},{"window_channel_isolation",window_channel_isolation},
        {"batch_pending",batch_pending},{"batch_missing",batch_missing},{"cache_hit",cache_hit},{"miss_becomes_hit",miss_becomes_hit},
        {"ordered_pages",ordered_pages},{"ordered_error",ordered_error},{"pending_order_diagnostic",pending_order_diagnostic},
        {"tx_cache_pressure",tx_cache_pressure},{"cache_rewrite",cache_rewrite},{"pending_frozen",pending_frozen},
        {"pending_tx_full",pending_tx_full},{"rejected_batch_no_effect",rejected_batch_no_effect},{"ingress_bypass",ingress_bypass},
        {"fault_fanout",fault_fanout},{"replacement_replay",replacement_replay},{"remap_cache",remap_cache},
        {"duplicate_handoff",duplicate_handoff},{"response_feedback",response_feedback},{"configuration",configuration},
        {"cache_pin_invalidation",cache_pin_invalidation},{"teardown",teardown},{"unsupported",unsupported},
        {"policies",policies},{"invalid_policy",invalid_policy},{"link_latency",link_latency},
        {"pending_recheck",pending_recheck},{"deadline_link_boundary",deadline_link_boundary}};
    try {
        if(argc!=2||!cases.count(argv[1]))throw std::runtime_error("choose stage2 scenario");
        cases.at(argv[1])();std::cout<<"PASS "<<argv[1]<<" ("<<checks<<" checks)\n";
    }catch(const std::exception&e){std::cerr<<"FAIL "<<(argc>1?argv[1]:"?")<<": "<<e.what()<<"\n";return 1;}
}

