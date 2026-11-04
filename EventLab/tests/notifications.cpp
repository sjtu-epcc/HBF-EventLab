#include "hbf_eventlab/runtime/read_path.hpp"
#include "hbf_eventlab/runtime/link_buffers.hpp"
#include "hbf_eventlab/runtime/simulation_session.hpp"
#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include "../examples/support/bounded_read_results.hpp"
#include <iostream>
#include <sstream>
using namespace hbf_eventlab;
namespace hbf_eventlab {
struct ReadPathTestAccess {
    // A normal accepted read's guard forbids an overlapping Pending write today.
    // Deliberately bypass that invariant ONLY to exercise the defensive terminal-
    // forwarding/result-space branch; no production admission uses this helper.
    static void remove_guard(ReadPath& r,ReadCandidate c) {
        auto& p=r.parents_.at({c.channel,c.command});
        r.capacity_.release_read(*p.guard,false);p.guard.reset();
    }
};
}
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
StaticDeviceDescription description(){Geometry g{2,1,1,8,4,1};return {g,{256,8,false,true},{256,Tick{1000000},16}};}
struct F {
    RuntimeProfile profile;
    StaticDeviceDescription d{description()};ReferenceAddressLayout layout{d.geometry()};
    ZoneMapping zones{d};BlockReplacement replacement{d.geometry()};NandMediaState media{d.geometry()};
    EventQueue events;AdmissionWaitRegistry signals{events};
    CommandAdmission admission{2,256,signals};CapacityRetirementCoordinator capacity{d,layout,zones,replacement,media};
    WritePath writes{d,layout,capacity,media,events,admission};
    std::unique_ptr<IReadResultStorage> storage;
    ReadPath reads;
    std::uint64_t serial=0;
    explicit F(bool shared=false):storage(shared?std::unique_ptr<IReadResultStorage>(new test_support::BoundedReadResults(1)):
        std::unique_ptr<IReadResultStorage>(new EgressBuffer(2,{1,64}))),reads(d,layout,capacity,media,admission,writes,*storage){signals.set_runtime_profile(profile);}
    ChannelAddress address(LogicalBlockKey b={},PageId p=0)const{return {b.channel,layout.encode_logical({b,p,0})};}
    HostCommand cmd(Op op=Op::Read,LogicalBlockKey b={},PageId p=0,std::uint32_t size=64){return {CommandId{++serial},op,address(b,p),size,0};}
    void committed(LogicalBlockKey b={}) {
        const auto s=writes.submit(cmd(Op::Write,b,0,4096),std::vector<DataToken>(64,DataToken{17}));
        check(s.admission==SubmitResult::Accepted&&!s.immediate_status,"normal initialization");
        for(auto c:writes.pending_erases())writes.finish_erase(writes.issue_erase(c.id)->id);
        const auto a=address(b);auto p=writes.program_candidate({b.channel,LocalDlu{a.local.value/dlu_bytes}});
        writes.finish_program(writes.issue_program(*p)->id);
        for(auto r:writes.responses())writes.response_delivered(r.channel,r.command);
        events.run();
    }
    ReadCandidate accept(HostCommand c){auto s=reads.submit(c);check(s.candidate.has_value(),"candidate accepted");return *s.candidate;}
    void until(Tick t){while(auto next=events.next_time()){if(t<*next)break;events.run_next();}}
    void finish(ReadCandidate c){auto op=reads.issue(c);check(op.has_value(),"issue");reads.finish_read(op->id);}
    void deliver(ReadCandidate c){reads.response_delivered(c.channel,c.command);}
    void drain(std::vector<ReadCandidate> cs){for(auto c:cs){finish(c);deliver(c);events.run();}
        check(admission.total_outstanding()==0&&reads.parent_count()==0&&capacity.logical_reads().size()==0&&signals.pending()==0,"component drain");}
};
void unrelated(const std::string& scope,unsigned count) {
    F f;LogicalBlockKey b{};
    if(scope=="zone")b.block=1;else if(scope=="other_zone")b.block=2;else if(scope=="channel")b.channel=1;
    f.committed();if(!(b==LogicalBlockKey{}))f.committed(b);
    auto first=f.accept(f.cmd());std::vector<ReadCandidate> others;
    for(unsigned i=0;i<count;++i)others.push_back(f.accept(f.cmd(Op::Read,b)));
    const auto before=f.profile.counter(RuntimeCounter::ReadWakeups);
    unsigned users=0,zone=0;
    f.signals.subscribe({f.signals.observe(WaitKey::physical(block_of(f.capacity.inspect(f.address()))))},[&]{++users;});
    f.signals.subscribe({f.signals.observe(f.capacity.zone_users_wait(f.address()))},[&]{++zone;});
    f.finish(first);f.events.run();
    check(f.storage->used_bytes()==64,"completion transfers ownership without freeing result");
    check(f.profile.wait(WaitReason::ResultSpace).notifications==0,"completion does not claim result space release");
    f.deliver(first);f.events.run();
    check(users==1&&zone==1,"drain notifications remain active");
    check(f.profile.counter(RuntimeCounter::ReadWakeups)==before,"unrelated candidate was not woken by releases");
    f.drain(others);
}
void unhealthy() {
    F f;f.committed();std::vector<ReadCandidate> cs;
    for(unsigned i=0;i<16;++i)cs.push_back(f.accept(f.cmd()));
    f.capacity.mark_unusable(block_of(f.capacity.inspect(f.address())));f.events.run();
    check(f.profile.counter(RuntimeCounter::ReadWakeResolved)==16,"health change wakes every affected reader");
    for(auto c:cs){auto r=f.reads.response(c.channel,c.command);check(r&&r->status==ReadStatus::TemporarilyRestricted,"read health result");f.deliver(c);}
    check(f.capacity.logical_reads().size()==0&&f.signals.pending()==0,"health change releases guards");
}
void release_each() {
    F f;auto a=f.capacity.pin(f.address()),b=f.capacity.pin(f.address());unsigned n=0;
    const auto key=WaitKey::physical(block_of(a->physical()));
    auto subscribe=[&]{f.signals.subscribe({f.signals.observe(key)},[&]{++n;});};
    subscribe();a.reset();f.capacity.publish_releases();f.events.run();check(n==1,"notify before last user exits");
    subscribe();b.reset();f.capacity.publish_releases();f.events.run();check(n==2,"notify last release too");
}
void zone_state(bool swap) {
    F f;const auto addr=f.address();f.capacity.declare_discardable(0,0);f.capacity.declare_discardable(0,1);
    check(f.capacity.quiesce_pair(0,0,1),"quiesce");
    auto c=f.cmd();auto rejected=f.reads.submit(c);check(rejected.admission==SubmitResult::Backpressured,"new read waits for zone state");
    unsigned wakes=0;f.signals.subscribe({f.signals.observe(rejected.waits.at(0))},[&]{++wakes;});
    if(swap)check(f.capacity.commit_swap(0,0,1,[]{})==RemapResult::Applied,"swap");else f.capacity.cancel_quiesce(0,0,1);
    f.events.run();check(wakes==1,"admission state wakes rejected read");
    auto s=f.reads.submit(c);check(s.admission==SubmitResult::Accepted,"read can now enter");
    if(s.candidate)f.finish(*s.candidate);f.reads.response_delivered(c.address.channel,c.id);
}
void pending_space(bool shared) {
    F f(shared);const LogicalBlockKey other{1,0,0,0};f.committed(other);
    auto occupied=f.accept(f.cmd(Op::Read,other));f.finish(occupied);
    // Fill channel 0 too for channel-local storage; shared pool is already full.
    if(!shared){f.committed();}
    std::optional<ReadCandidate> local;
    if(!shared){local=f.accept(f.cmd());f.finish(*local);}
    // Different block permits the Pending write while old responses remain held.
    LogicalBlockKey b{0,0,0,1};auto w=f.cmd(Op::Write,b);f.writes.submit(w,{DataToken{71}});
    auto r=f.cmd(Op::Read,b);auto s=f.reads.submit(r);check(s.admission==SubmitResult::Backpressured,"Pending forwarding waits for real space");
    check(f.admission.outstanding(0)==(shared?1:2),"unaccepted forwarding has no credit");
    unsigned n=0;f.signals.subscribe({f.signals.observe(s.waits.at(0))},[&]{++n;});
    f.deliver(occupied);f.until(Tick{0});
    check(n==(shared?1u:0u),"wake scope matches actual pool");
    if(local){f.deliver(*local);f.until(Tick{0});check(n==1,"own channel release wakes forwarding");}
    auto ready=f.reads.submit(r);check(ready.disposition==ReadDisposition::Forwarded,"forward resumes");
    check(f.reads.response(0,r.id)->data==DataToken{71},"forwarded data retained");f.reads.response_delivered(0,r.id);
}
void entry_budget() {
    F f;auto& storage=dynamic_cast<EgressBuffer&>(*f.storage);const auto slot=storage.reserve_metadata(0);
    check(storage.used_bytes()==0&&!storage.can_reserve(0)&&storage.can_reserve(1),"metadata entry blocks only its channel");
    auto c=f.accept(f.cmd());check(f.reads.probe(c).state==ProbeState::Blocked,"probe checks entries as well as bytes");
    storage.release(*slot);check(f.reads.probe(c).state==ProbeState::Ready,"entry restored");f.drain({c});
}
void terminal_wait(bool become_error) {
    F f;f.committed();auto held=f.accept(f.cmd());f.finish(held);
    auto c=f.accept(f.cmd());ReadPathTestAccess::remove_guard(f.reads,c);
    auto w=f.cmd(Op::Write);f.writes.submit(w,{DataToken{91}});
    check(!f.reads.resolve_terminal(c),"terminal data blocked on result space");
    f.until(Tick{0});check(f.profile.counter(RuntimeCounter::ReadResultBlocked)>0,"explicit terminal-space outcome measured");
    if(become_error){f.capacity.mark_unusable(block_of(f.capacity.inspect(f.address())));f.until(Tick{1000000});
        const auto r=f.reads.response(0,c.command);check(r&&r->status==ReadStatus::TemporarilyRestricted,"state change bypasses full result storage");}
    else {f.deliver(held);f.until(Tick{0});check(f.reads.response(0,c.command)->data==DataToken{91},"space release resolves terminal data");}
    f.deliver(c);if(become_error)f.deliver(held);
}
void registry() {
    RuntimeProfile p;EventQueue e;AdmissionWaitRegistry s(e);s.set_runtime_profile(p);
    const auto a=WaitKey::channel_key(WaitReason::Credit,0),b=WaitKey::channel_key(WaitReason::ResultSpace,0);
    unsigned n=0;auto id=s.subscribe({s.observe(a),s.observe(b)},[&]{++n;});
    s.notify(a);s.notify(b);s.notify(a);check(s.queued_wakeups()==1,"coalesced one-shot");
    s.cancel(id);e.run();check(!n&&p.wait(WaitReason::Credit).cancelled==1,"cancel queued callback");
    auto old=s.observe(b);s.notify(b);s.subscribe({old},[&]{++n;});e.run();
    check(n==1&&p.wait(WaitReason::ResultSpace).revision_wakes==1,"revision race counted");
    check(p.wait(WaitReason::Credit).queued==1&&p.wait(WaitReason::Credit).subscriber_hits==1,"hits exclude disarmed subscriptions");
    check(p.wait(WaitReason::ResultSpace).callbacks==1&&p.measurement(RuntimeRegion::WaitCallback).calls==1,"first-trigger attribution");
    std::ostringstream out;p.write_json(out);check(out.str().find("wait_reasons")!=std::string::npos,"export reason aggregates");
}
void reserved_rollback(bool cached) {
    F f;f.committed();auto c=f.accept(f.cmd());ReadPathTestAccess::remove_guard(f.reads,c);
    f.writes.submit(f.cmd(Op::Write),{DataToken{92}});
    const auto slot=f.storage->reserve(0);check(slot.has_value(),"temporary result reservation");
    ReadResultLease data(*f.storage,*slot);data.store(DataToken{11});
    if(cached)check(!f.reads.serve_cached(c,std::move(data)),"terminal state prevents cached service");
    else check(!f.reads.issue_reserved(c,std::move(data)),"terminal state prevents media issue");
    check(!f.reads.response(0,c.command),"result wake is asynchronous");
    f.until(Tick{0});const auto r=f.reads.response(0,c.command);
    check(r&&r->data==DataToken{92},"rollback release wakes real terminal-space waiter");
    check(f.reads.active_reads()==0,"no fabricated media operation");f.deliver(c);
}
void metadata_release() {
    RuntimeProfile p;SimulationConfiguration c;c.device.geometry.host_channels=2;
    c.execution.buffers.egress={1,64};c.execution.ucie.overhead.response_fixed_latency=Tick{100000};
    c.device.selected.assembly_timeout=Tick{10000000};c.host.max_timeout_retries=0;c.host.hints=ReadHintMode::AllRegular;
    SimulationSession s(resolve_configuration(c),nullptr,&p);
    // 64B interleaving: all three commands address channel 1. The missing
    // Pending fragment produces a metadata-only 0xA response occupying TX.
    GlobalRequest w{GlobalRequestId{1},Op::Write,GlobalByteAddress{64},64,1};
    GlobalRequest missing{GlobalRequestId{2},Op::Read,GlobalByteAddress{192},64};
    GlobalRequest hit{GlobalRequestId{3},Op::Read,GlobalByteAddress{64},64};hit.expected_content=1;
    s.submit(std::vector<GlobalRequest>{w,missing,hit});s.close_input();const auto r=s.run_to_completion();
    check(r.run.status==RunStatus::Completed&&r.run.host.drained()&&r.run.device.drained(),"metadata/forward/timeout drain");
    check(s.host().results().at(GlobalRequestId{2}).state==LogicalResult::Failed&&
          s.host().results().at(GlobalRequestId{3}).state==LogicalResult::Success,"missing error and later forwarded data");
    check(p.wait(WaitReason::ResultSpace).callbacks>0,"metadata entry release wakes actual channel-local waiter");
}
}
int main(int argc,char**argv){try{
    if(argc!=2)throw std::invalid_argument("scenario required");const std::string name=argv[1];
    if(name=="same_block")unrelated("block",8);else if(name=="same_block_many")unrelated("block",128);
    else if(name=="same_zone")unrelated("zone",32);else if(name=="other_zone")unrelated("other_zone",32);
    else if(name=="channels")unrelated("channel",32);else if(name=="health")unhealthy();
    else if(name=="release_each")release_each();else if(name=="zone_cancel")zone_state(false);else if(name=="zone_swap")zone_state(true);
    else if(name=="shared_space")pending_space(true);else if(name=="local_space")pending_space(false);
    else if(name=="entry_budget")entry_budget();else if(name=="terminal_space")terminal_wait(false);
    else if(name=="terminal_error")terminal_wait(true);else if(name=="registry")registry();
    else if(name=="reserved_rollback")reserved_rollback(false);else if(name=="cached_rollback")reserved_rollback(true);
    else if(name=="metadata_release")metadata_release();
    else throw std::invalid_argument("unknown scenario");
    std::cout<<"PASS notification "<<name<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
