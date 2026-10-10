#include "hbf_eventlab/runtime/read_path.hpp"
#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include "hbf_eventlab/policies/read_dispatch.hpp"
#include "../examples/support/bounded_read_results.hpp"
#include <algorithm>
#include <iostream>
#include <string>

using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool value,const char* why) { ++checks; if(!value) throw std::runtime_error(why); }
template<class E=std::exception,class F> void rejects(F f) {
    bool caught=false; try{f();}catch(const E&){caught=true;} check(caught,"expected typed diagnostic");
}
StaticDeviceDescription profile(std::uint32_t mocs=256) {
    Geometry g{2,2,2,8,3,1};
    return {g,{256,8,false,true},{mocs,Tick{100},g.block_group_dlus()}};
}
struct F {
    StaticDeviceDescription d;
    ReferenceAddressLayout layout;
    ZoneMapping zones;
    BlockReplacement replacement;
    NandMediaState media;
    EventQueue events;
    AdmissionWaitRegistry signals{events};
    CommandAdmission admission;
    CapacityRetirementCoordinator capacity;
    WritePath writes;
    test_support::BoundedReadResults storage;
    ReadPath reads;
    LogicalBlockKey b{0,0,0,0};
    std::uint64_t serial=0;
    explicit F(std::size_t result_slots=8,std::uint32_t mocs=256)
        :d(profile(mocs)),layout(d.geometry()),zones(d),replacement(d.geometry()),media(d.geometry()),
         admission(2,mocs,signals),capacity(d,layout,zones,replacement,media),
         writes(d,layout,capacity,media,events,admission),storage(result_slots),
         reads(d,layout,capacity,media,admission,writes,storage) {}
    ChannelAddress address(PageId page=0,std::uint32_t slot=0) const {
        return {b.channel,layout.encode_logical({b,page,slot*64})};
    }
    DluKey key(PageId page=0) const { return {b.channel,LocalDlu{address(page).local.value/dlu_bytes}}; }
    HostCommand cmd(Op op=Op::Read,PageId page=0,std::uint32_t slot=0,std::uint32_t bytes=64) {
        return {CommandId{++serial},op,address(page,slot),bytes,0};
    }
    WriteSubmission write(PageId page=0,std::uint32_t slot=0,std::uint32_t count=64,std::uint64_t value=17) {
        return writes.submit(cmd(Op::Write,page,slot,count*64),std::vector<DataToken>(count,DataToken{value}));
    }
    void erase() {
        for(auto c:writes.pending_erases()) {
            const auto op=writes.issue_erase(c.id); check(op.has_value(),"explicit erase issue");
            writes.finish_erase(op->id);
        }
    }
    ProgramOperation program(PageId page=0) {
        const auto c=writes.program_candidate(key(page)); check(c.has_value(),"program candidate exists");
        const auto op=writes.issue_program(*c); check(op.has_value(),"legal program issue"); return *op;
    }
    void deliver_writes() { for(auto r:writes.responses()) writes.response_delivered(r.channel,r.command); }
    void committed(PageId page=0,std::uint64_t value=17) {
        const auto s=write(page,0,64,value); check(s.admission==SubmitResult::Accepted && !s.immediate_status,"write accepted");
        erase(); writes.finish_program(program(page).id); deliver_writes();
    }
    ReadCandidate accept(HostCommand c) {
        const auto s=reads.submit(c);
        check(s.admission==SubmitResult::Accepted && s.candidate.has_value(),"read accepted as candidate"); return *s.candidate;
    }
    ReadOperation issue(ReadCandidate c) {
        const auto op=reads.issue(c); check(op.has_value(),"read issue"); return *op;
    }
    ReadResponse response(CommandId id) {
        for(auto r:reads.responses()) if(r.command==id) return r;
        throw std::runtime_error("response missing");
    }
    PhysicalBlockKey physical() const { return block_of(capacity.inspect(address())); }
};
void event_rounds() {
    EventQueue q; std::vector<int> out;
    q.schedule_at(Tick{10},EventPhase::Deadline,[&]{out.push_back(4);});
    q.schedule_at(Tick{10},EventPhase::Arrival,[&] {
        out.push_back(1);
        q.schedule_at(q.now(),EventPhase::Completion,[&] {
            check(q.round()==1,"backward phase moves to next round"); out.push_back(2);
            q.schedule_at(q.now(),EventPhase::Arrival,[&]{out.push_back(3);});
        });
    });
    q.run(); check(out==std::vector<int>({1,2,3,4}),"normal causal closure before deadline");
    check(q.now()==Tick{10} && q.executed()==4,"no fabricated time advance");
    rejects<std::invalid_argument>([&]{q.schedule_at(Tick{9},EventPhase::Arrival,[]{});});
    q.schedule_at(Tick{11},EventPhase::Arrival,[&]{rejects<std::logic_error>([&]{q.run();});});
    q.run();
}
void deadline_batch() {
    for(bool reverse:{false,true}) {
        EventQueue q; int expired=0,feedback=0;
        auto add=[&](int) {q.schedule_at(Tick{8},EventPhase::Deadline,[&] {
            ++expired;
            q.schedule_at(q.now(),EventPhase::Completion,[&] {
                check(expired==2,"whole deadline cohort committed before feedback"); ++feedback;
            });
        });};
        add(reverse?2:1); add(reverse?1:2);
        q.run(); check(expired==2 && feedback==2,"registration order cannot rescue cohort");
    }
}
void deadline_limits() {
    EventQueue q; int expired=0;
    q.set_limits({1,100,100});
    q.schedule_at(Tick{4},EventPhase::Deadline,[&]{++expired;});
    q.schedule_at(Tick{4},EventPhase::Deadline,[&]{++expired;});
    rejects<EventLimitReached>([&]{q.run();});
    check(expired==0 && q.pending()==2,"batch budget checked before any termination");
    q.set_limits({2,100,100}); q.run(); check(expired==2 && q.pending()==0,"resume frozen batch");
    EventQueue normal; int n=0; bool deadline=false;
    normal.set_limits({0,3,100});
    std::function<void()> loop=[&]{++n;normal.schedule_at(normal.now(),EventPhase::Arrival,loop);};
    normal.schedule_at(Tick{7},EventPhase::Arrival,loop);
    normal.schedule_at(Tick{7},EventPhase::Deadline,[&]{deadline=true;});
    rejects<EventLimitReached>([&]{normal.run();});
    check(n==3 && !deadline && normal.now()==Tick{7},"livelock limit does not force timeout or add ticks");
}
void deadline_exception() {
    EventQueue q; std::vector<int> out;
    q.schedule_at(Tick{5},EventPhase::Deadline,[&]{
        out.push_back(1);
        q.schedule_at(q.now(),EventPhase::Arrival,[&]{out.push_back(3);});
        throw std::runtime_error("injected callback failure");
    });
    q.schedule_at(Tick{5},EventPhase::Deadline,[&]{out.push_back(2);});
    rejects<std::runtime_error>([&]{q.run_next();});
    check(q.pending()==2 && q.settling_deadlines(),"remaining batch retained across exception");
    q.run(); check(out==std::vector<int>({1,2,3}),"resumption cannot interleave feedback");
}
void event_cancel_limits() {
    EventQueue q; q.set_limits({1,1,1});
    auto a=q.schedule_at(Tick{6},EventPhase::Deadline,[]{throw std::runtime_error("cancelled");});
    auto b=q.schedule_at(Tick{6},EventPhase::Deadline,[]{});
    check(q.cancel(a) && !q.cancel(a),"cancel exactly once");
    q.run(); check(q.executed()==1 && !q.cancel(b),"cancelled event not billed");
    EventQueue rounds; rounds.set_limits({0,100,2});
    std::function<void()> f=[&]{rounds.schedule_at(rounds.now(),EventPhase::Completion,f);};
    rounds.schedule_at(Tick{0},EventPhase::Completion,f);
    rejects<EventLimitReached>([&]{rounds.run();});
    check(rounds.executed()==2 && rounds.pending()==1,"same-phase recursion bounded by rounds");
}
void wait_targeting() {
    EventQueue e; AdmissionWaitRegistry waits(e); int a=0,b=0;
    const auto ka=WaitKey::channel_key(WaitReason::Credit,0),kb=WaitKey::channel_key(WaitReason::Credit,1);
    auto wa=waits.subscribe({waits.observe(ka)},[&]{++a;});
    auto wb=waits.subscribe({waits.observe(kb)},[&]{++b;});
    waits.notify(ka); waits.notify(ka);
    check(a==0 && b==0 && waits.queued_wakeups()==1,"coalesced and deferred notification");
    e.run(); check(a==1 && b==0,"only target channel wakes");
    check(!waits.cancel(wa) && waits.cancel(wb) && waits.pending()==0,"one-shot and cancellation");
}
void wait_race_cancel() {
    EventQueue e; AdmissionWaitRegistry waits(e); int n=0;
    auto a=WaitKey::channel_key(WaitReason::Credit,0),b=WaitKey::channel_key(WaitReason::ResultSpace,0);
    auto observed=waits.observe(a); waits.notify(a);
    auto id=waits.subscribe({observed,waits.observe(b)},[&]{++n;});
    waits.notify(b); check(waits.queued_wakeups()==1,"one callback for OR conditions");
    check(waits.cancel(id),"cancel queued wakeup"); e.run(); check(n==0,"late cancelled event harmless");
    auto old=waits.observe(a); waits.notify(a);
    waits.subscribe({old},[&]{++n;}); e.run(); check(n==1,"revision prevents lost wakeup");
    {
        AdmissionWaitRegistry temporary(e);
        temporary.subscribe({temporary.observe(a)},[&]{++n;}); temporary.notify(a);
    }
    e.run(); check(n==1,"registry teardown cancels deferred callbacks");
}
void wait_reregister() {
    EventQueue e; AdmissionWaitRegistry waits(e); int n=0;
    auto key=WaitKey::channel_key(WaitReason::Credit,0);
    waits.subscribe({waits.observe(key)},[&]{
        ++n; waits.subscribe({waits.observe(key)},[&]{++n;});
    });
    waits.notify(key); e.run(); check(n==1 && waits.pending()==1,"subscription alone is not an active progress event");
    waits.notify(key); e.run(); check(n==2 && waits.pending()==0,"retry explicitly rearms");
}
void read_accept_issue() {
    F f; f.committed();
    const auto cmd=f.cmd(); const auto c=f.accept(cmd);
    check(f.reads.waiting()==1 && f.reads.active_reads()==0 && f.storage.used_bytes()==0,"accept does not issue or sample data");
    check(f.capacity.in_flight(f.physical())==0 && f.capacity.logical_reads().size()==1,"logical guard without physical pin");
    for(int i=0;i<10;++i) check(f.reads.probe(c).state==ProbeState::Ready,"read-only probe");
    check(f.capacity.in_flight(f.physical())==0 && f.storage.used_bytes()==0,"probe has no allocation side effects");
    check(f.write().admission==SubmitResult::Backpressured,"queued old content blocks rewrite");
    const auto op=f.issue(c);
    check(f.storage.used_bytes()==64 && f.capacity.in_flight(f.physical())==1,"issue reserves storage and pins physical target");
    check(!f.reads.issue(c),"duplicate issue rejected");
    rejects([&]{f.reads.response_delivered(0,cmd.id);});
    f.reads.finish_read(op.id);
    check(f.capacity.logical_reads().size()==0 && f.capacity.in_flight(f.physical())==0,"stable result releases content and lease");
    check(f.admission.outstanding(0)==1 && f.storage.used_bytes()==64,"completion retains credit and result");
    f.committed(0,99);
    check(f.response(cmd.id).data==DataToken{17},"response survives overwrite");
    f.reads.response_delivered(0,cmd.id);
    check(f.storage.used_bytes()==0 && f.admission.outstanding(0)==0,"delivery frees bounded storage and credit once");
    rejects([&]{f.reads.finish_read(op.id);});
    rejects([&]{f.reads.response_delivered(0,cmd.id);});
}
void read_bounded_results() {
    F f(1); f.committed(); auto a=f.cmd(),b=f.cmd();
    auto ca=f.accept(a),cb=f.accept(b); auto op=f.issue(ca);
    check(f.reads.probe(cb).state==ProbeState::Blocked && !f.reads.issue(cb),"full bounded owner blocks issue");
    check(f.capacity.in_flight(f.physical())==1 && f.reads.active_reads()==1,"failed issue no extra lease");
    f.reads.finish_read(op.id); check(!f.reads.issue(cb),"completed result still occupies storage until delivered");
    f.reads.response_delivered(0,a.id);
    auto second=f.issue(cb); f.reads.finish_read(second.id);
    check(f.response(b.id).data==DataToken{17},"other candidate progresses after release");
    f.reads.response_delivered(0,b.id);
}
void read_order() {
    F f; f.committed(0,10); f.committed(1,20);
    auto a=f.cmd(),b=f.cmd(Op::Read,1),independent=f.cmd(Op::Read,1);
    a.read_group=7;b.read_group=7;
    auto ca=f.accept(a),cb=f.accept(b),cc=f.accept(independent);
    check(!f.reads.issue(cb) && f.reads.probe(cb).state==ProbeState::Blocked,"same group different page waits for delivery");
    const auto independent_op=f.issue(cc); f.reads.finish_read(independent_op.id);
    f.reads.response_delivered(0,independent.id);
    const auto first=f.issue(ca); f.reads.finish_read(first.id);
    check(!f.reads.issue(cb),"physical completion alone does not advance group");
    bool woke=false;f.signals.subscribe({f.signals.observe(WaitKey::order(0,7))},[&]{woke=true;});
    f.reads.response_delivered(0,a.id); check(!woke,"order wakeup deferred"); f.events.run(); check(woke,"order wakeup published");
    const auto second=f.issue(cb);f.reads.finish_read(second.id);f.reads.response_delivered(0,b.id);
    check(f.admission.read_order().pending()==0,"group state drained");
}
void read_order_errors() {
    F f; auto a=f.cmd(),bad=f.cmd(),next=f.cmd();a.read_group=9;bad.read_group=9;next.read_group=9;
    bad.address.local=LocalByteAddress{UINT64_MAX};
    auto ca=f.accept(a);
    check(f.reads.submit(bad).immediate_status==ReadStatus::InvalidAddress,"ordinary error is accepted");
    auto cn=f.accept(next);
    check(f.reads.responses().empty(),"ready error waits for earlier group head");
    rejects([&]{f.reads.response_delivered(0,bad.id);});
    f.reads.finish_read(f.issue(ca).id);
    check(f.response(a.id).status==ReadStatus::UnwrittenPage,"head can be an error");
    f.reads.response_delivered(0,a.id);
    check(f.response(bad.id).status==ReadStatus::InvalidAddress,"first error advances to queued error");
    f.reads.response_delivered(0,bad.id);
    f.reads.finish_read(f.issue(cn).id);f.reads.response_delivered(0,next.id);
    check(f.admission.total_outstanding()==0,"errors do not strand order groups");
}
void pending_order_diagnostic() {
    F f; f.write(0,0,1);
    for(unsigned slot:{0u,1u}) {
        auto c=f.cmd(Op::Read,0,slot);c.read_group=0;const auto n=f.admission.total_outstanding();
        rejects<std::invalid_argument>([&]{f.reads.submit(c);});
        check(f.admission.total_outstanding()==n && f.storage.used_bytes()==0 &&
              f.capacity.logical_reads().size()==0 && f.admission.read_order().pending()==0,
              "ordered Pending input rejected before all side effects");
    }
}
void pending_capacity() {
    F f(1); f.write(0,0,1,41);
    auto a=f.cmd(); check(f.reads.submit(a).disposition==ReadDisposition::Forwarded,"Pending uses bounded result owner");
    auto b=f.cmd();const auto before=f.admission.total_outstanding();
    auto s=f.reads.submit(b); check(s.admission==SubmitResult::Backpressured && !s.candidate,"full result storage defers before acceptance");
    check(f.admission.total_outstanding()==before,"no credit on unaccepted forwarding");
    auto missing=f.cmd(Op::Read,0,1);check(f.reads.submit(missing).immediate_status==ReadStatus::PendingWriteMissing,"metadata error needs no payload slot");
    f.erase();f.events.run();f.deliver_writes();
    check(f.response(a.id).data==DataToken{41} && f.response(missing.id).status==ReadStatus::PendingWriteMissing,
          "timeout cannot reinterpret saved Pending results");
    f.reads.response_delivered(0,a.id);f.reads.response_delivered(0,missing.id);
    check(f.storage.used_bytes()==0,"forwarding storage released");
}
void read_failure_revalidation() {
    F f; f.committed(); const auto cmd=f.cmd(); auto c=f.accept(cmd);
    f.write(1); const auto op=f.program(1);
    f.writes.finish_program(op.id,ProgramOutcome::FailedReusable);
    check(f.reads.probe(c).state==ProbeState::Terminal,"queued read observes failure without NAND issue");
    f.events.run();
    check(f.response(cmd.id).status==ReadStatus::TemporarilyRestricted && f.capacity.logical_reads().size()==0,
          "targeted progress fixes terminal response and frees dependency");
    check(f.reads.active_reads()==0 && !f.reads.issue(c),"no fake media operation for queued error");
    f.deliver_writes();
    check(f.write().admission==SubmitResult::Accepted,"error response waiting for delivery does not deadlock replay");
}
void read_repair_revalidation() {
    F f; f.committed();const auto cmd=f.cmd();f.accept(cmd);
    f.capacity.mark_unusable(f.physical());
    check(f.capacity.repair_block(f.b,[]{})==CapacityResult::InFlight,"repair cannot cross queued logical reader");
    f.events.run();
    check(f.response(cmd.id).restriction==ReadRestrictionReason::PhysicalRepairRequired,"unissued read can fail without physical lease");
    check(f.capacity.repair_block(f.b,[]{})==CapacityResult::Replaced,"released dependency allows repair");
    check(f.response(cmd.id).status==ReadStatus::TemporarilyRestricted,"fixed error not reinterpreted after repair");
}
void read_quiesce_drain() {
    F f;f.committed();const auto cmd=f.cmd();auto c=f.accept(cmd);
    f.capacity.declare_discardable(0,0);f.capacity.declare_discardable(0,1);f.capacity.quiesce_pair(0,0,1);
    check(f.capacity.commit_swap(0,0,1,[]{})==RemapResult::InFlight,"logical guard blocks swap");
    check(f.reads.submit(f.cmd()).admission==SubmitResult::Backpressured,"new reads cannot enter quiesced zone");
    auto op=f.issue(c); f.reads.finish_read(op.id);
    check(f.capacity.commit_swap(0,0,1,[]{})==RemapResult::Applied,"old queued reader can drain during quiesce");
    check(f.response(cmd.id).data==DataToken{17},"saved response survives remap");
}
void read_stale_candidate() {
    F f;const auto cmd=f.cmd();const auto old=f.accept(cmd);
    const auto op=f.issue(old);f.reads.finish_read(op.id);f.reads.response_delivered(0,cmd.id);
    const auto current=f.accept(cmd);
    check(old.identity!=current.identity && !f.reads.issue(old),"reused command ID does not revive old candidate");
    f.reads.finish_read(f.issue(current).id);f.reads.response_delivered(0,cmd.id);
    rejects([&]{f.reads.finish_read(op.id);});
    f.events.run();check(f.reads.parent_count()==0 && f.signals.pending()==0,"late queued callbacks removed");
}
void write_probes() {
    F f; f.write();const auto erase=f.writes.pending_erases().front();
    const auto program=*f.writes.program_candidate(f.key());
    for(int i=0;i<10;++i) {
        check(f.writes.probe_erase(erase.id).state==ProbeState::Ready,"erase candidate remains ready");
        check(f.writes.probe_program(program).state==ProbeState::Blocked,"erase not yet complete");
    }
    check(f.writes.active_erases()==0 && f.media.erase_count(f.physical())==0,"query does not start/finish erase");
    rejects([&]{f.writes.finish_erase(OperationId{erase.id.value});});
    check(!f.writes.issue_program(program) && f.writes.find(f.key())->phase==WritePhase::Ready,"failed issue does not mark Programming");
    const auto issued=f.writes.issue_erase(erase.id); check(issued.has_value(),"actual erase issue");
    check(!f.writes.issue_erase(erase.id),"no duplicate erase issue");
    f.writes.finish_erase(issued->id);
    const auto n=f.writes.order(f.b).next_page_to_program();
    for(int i=0;i<10;++i)check(f.writes.probe_program(program).state==ProbeState::Ready,"ready probe stable");
    check(f.writes.order(f.b).next_page_to_program()==n && f.writes.active_programs()==0,"program query is pure");
    const auto op=f.writes.issue_program(program);check(op.has_value() && !f.writes.issue_program(program),"single controlled issue");
    f.writes.finish_program(op->id);f.deliver_writes();
    rejects([&]{f.writes.finish_erase(issued->id);});
}
void program_stale_candidate() {
    F f; f.write(0,0,1); const auto old=*f.writes.program_candidate(f.key());
    f.erase();f.events.run();f.deliver_writes();f.write();
    const auto current=*f.writes.program_candidate(f.key());
    check(old.context!=current.context && f.writes.probe_program(old).state==ProbeState::Stale,"DLU retry changes context");
    check(!f.writes.issue_program(old) && f.writes.find(f.key())->phase==WritePhase::Ready,"stale query cannot issue new data");
    f.writes.finish_program(f.program().id);f.deliver_writes();
}
void write_ready_notifications() {
    F f;f.write();f.erase();f.write(1);
    const auto second=*f.writes.program_candidate(f.key(1));
    check(f.writes.probe_program(second).state==ProbeState::Blocked,"earlier page must succeed");
    int n=0;f.signals.subscribe({f.signals.observe(WaitKey::block_key(WaitReason::BlockProgress,f.b))},[&]{
        ++n;check(f.writes.probe_program(second).state==ProbeState::Ready,"wake observes complete transition");
    });
    f.writes.finish_program(f.program().id);
    check(n==0,"no reentrant scheduling during finish_program");f.events.run();check(n==1,"one targeted block progress wake");
    f.writes.finish_program(f.program(1).id);f.deliver_writes();
}
void physical_release_notification() {
    F f;auto lease=f.capacity.pin(f.address());int n=0;
    f.signals.subscribe({f.signals.observe(WaitKey::physical(f.physical()))},[&]{++n;});
    lease.reset();check(n==0 && f.events.pending()==0,"lease destructor does not publish callbacks");
    f.capacity.publish_releases();check(n==0 && f.events.pending()==1,"stable boundary publishes progress");
    f.events.run();check(n==1,"physical release wakes its subscribers");
}
void deadline_credit_chain() {
    F f; // 63 fragments + 193 completed errors fill 256 MOCS credits.
    for(unsigned i=0;i<63;++i)f.write(0,i,1);
    std::vector<CommandId> errors;
    for(unsigned i=0;i<193;++i) {
        auto c=f.cmd();c.address.local=LocalByteAddress{UINT64_MAX};errors.push_back(c.id);f.reads.submit(c);
    }
    check(f.admission.outstanding(0)==256,"credit full at deadline");
    const auto last=f.cmd(Op::Write,0,63);
    check(f.writes.submit(last,{DataToken{17}}).admission==SubmitResult::Backpressured,"last fragment initially unaccepted");
    int wakes=0;
    f.signals.subscribe({f.signals.observe(WaitKey::channel_key(WaitReason::Credit,0))},[&]{
        ++wakes;
        f.events.schedule_at(f.events.now(),EventPhase::Completion,[&]{
            f.events.schedule_at(f.events.now(),EventPhase::Arrival,[&] {
                check(f.writes.submit(last,{DataToken{17}}).admission==SubmitResult::Accepted,"credit feedback admits fragment");
            });
        });
    });
    f.events.schedule_at(Tick{100},EventPhase::Completion,[&]{f.reads.response_delivered(0,errors.front());});
    f.events.run();
    check(wakes==1 && f.writes.find(f.key())->phase==WritePhase::Ready && f.writes.responses().empty(),
          "multiple causal rounds beat common deadline, without added ticks");
}
void deadline_resident_only() {
    F f(8,1);f.write(0,0,1);
    bool reached_rx=false;
    const auto late=f.cmd(Op::Write,0,1);
    f.events.schedule_at(Tick{100},EventPhase::Arrival,[&]{
        reached_rx=true;check(f.writes.submit(late,{DataToken{9}}).admission==SubmitResult::Backpressured,"RX arrival still needs credit");
    });
    f.events.run();
    check(reached_rx && !f.writes.find(f.key()) && f.writes.responses().front().status==WriteStatus::AssemblyTimeout,
          "only formal acceptance changes bitmap; RX is a test marker, not modeled hardware");
}
void deadline_two_dlus() {
    for(bool reverse:{false,true}) {
        F f;LogicalBlockKey a{0,0,0,0},b{0,0,1,0};
        f.b=reverse?b:a;f.write(0,0,1);
        f.b=reverse?a:b;f.write(0,0,1);
        const auto ka=DluKey{0,LocalDlu{f.layout.encode_logical({a,0,0}).value/dlu_bytes}};
        const auto kb=DluKey{0,LocalDlu{f.layout.encode_logical({b,0,0}).value/dlu_bytes}};
        int observed=0;
        f.signals.subscribe({f.signals.observe(WaitKey::dlu(ka))},[&] {
            ++observed;check(!f.writes.find(ka) && !f.writes.find(kb),"both contexts gone before timeout notification");
        });
        f.events.run();check(observed==1 && f.writes.responses().size()==2,"cohort independent of registration order");
        for(auto r:f.writes.responses())check(r.status==WriteStatus::AssemblyTimeout,"both old attempts timed out");
    }
}
void window_membership() {
    WindowDispatchPolicy p(Tick{10});ReadDispatchWindow window(p);
    window.arrive(CommandId{1},ReadHint::Regular,Tick{0});
    const auto stale=*window.wakeup();
    auto batches=window.accept({},ReadHint::Batch,Tick{1});
    check(batches.size()==1 && batches[0]==DispatchBatch{CommandId{1}},"forwarded Batch closes old members only");
    check(window.accept({},ReadHint::Batch,Tick{1}).empty(),"empty Batch creates no fake NAND request");
    window.arrive(CommandId{2},ReadHint::Regular,Tick{2});
    check(!window.flush_due(stale,Tick{20}),"old timer cannot flush reused window even late");
    const auto fresh=*window.wakeup();check(window.flush_due(fresh,Tick{20})->size()==1,"matching timer releases tail");
    window.arrive(CommandId{3},ReadHint::Regular,Tick{21});
    rejects([&]{window.arrive(CommandId{3},ReadHint::Batch,Tick{21});});
    check(window.remove(CommandId{3}) && !window.remove(CommandId{3}) && !window.wakeup(),"terminal member removal");
}
void group_channel_isolation() {
    F f;auto a=f.cmd();a.read_group=4;auto ca=f.accept(a);
    f.b.channel=1;auto b=f.cmd();b.id=a.id;b.read_group=4;auto cb=f.accept(b);
    check(f.reads.probe(ca).state==ProbeState::Ready && f.reads.probe(cb).state==ProbeState::Ready,"group and command identities scoped by channel");
    f.reads.finish_read(f.issue(cb).id);f.reads.response_delivered(1,b.id);
    f.reads.finish_read(f.issue(ca).id);f.reads.response_delivered(0,a.id);
}
void teardown_queued_read() {
    F f;const auto cmd=f.cmd();
    {
        ReadPath temporary(f.d,f.layout,f.capacity,f.media,f.admission,f.writes,f.storage);
        temporary.submit(cmd);
        check(f.capacity.logical_reads().size()==1 && f.signals.pending()==1,"queued reader owns one guard and subscription");
        f.capacity.mark_unusable(f.physical()); // queued wakeup must be cancelled by teardown.
    }
    f.events.run();check(f.admission.total_outstanding()==0 && f.capacity.logical_reads().size()==0 &&
                         f.signals.pending()==0 && f.storage.used_bytes()==0,"teardown cancels only owned transient work");
}

class FaultingResults final : public IReadResultStorage {
public:
    test_support::BoundedReadResults backing{1};
    bool fail_store=true;
    std::optional<ReadResultId> reserve() override {return backing.reserve();}
    void store(ReadResultId id,DataToken d) override {
        if(fail_store)throw std::runtime_error("injected result-owner failure");
        backing.store(id,d);
    }
    DataToken load(ReadResultId id) const override {return backing.load(id);}
    void release(ReadResultId id) noexcept override {backing.release(id);}
    std::size_t used_bytes() const override {return backing.used_bytes();}
    std::size_t capacity_bytes() const override {return backing.capacity_bytes();}
};
void issue_rollback() {
    F f;f.committed();
    FaultingResults storage;
    ReadPath reader(f.d,f.layout,f.capacity,f.media,f.admission,f.writes,storage);
    const auto cmd=f.cmd();const auto c=*reader.submit(cmd).candidate;
    rejects<std::runtime_error>([&]{reader.issue(c);});
    check(reader.waiting()==1 && reader.active_reads()==0 && storage.used_bytes()==0 &&
          f.capacity.in_flight(f.physical())==0 && f.capacity.logical_reads().size()==1,
          "result failure rolls back reserved data/physical pin, retaining accepted logical request");
    storage.fail_store=false;
    const auto op=reader.issue(c);check(op.has_value(),"same candidate can issue after failure removed");
    reader.finish_read(op->id);reader.response_delivered(0,cmd.id);
    check(f.admission.total_outstanding()==0,"no leaked ledger after rollback/retry");
}
void issue_sampling() {
    F f;
    auto cmd=f.cmd();auto c=f.accept(cmd);
    // Deliberate low-level fixture mutation, NOT a legal bypass for production writes:
    // proves the functional media read occurs at issue, not at submit.
    PageData data{};data.fill(DataToken{51});
    auto target=f.capacity.inspect(f.address());f.media.program(target,data);
    auto op=f.issue(c);f.reads.finish_read(op.id);
    check(f.response(cmd.id).data==DataToken{51},"media result sampled at explicit issue");
}
void guard_multiple_readers() {
    F f;f.committed();auto a=f.cmd(),b=f.cmd();auto ca=f.accept(a),cb=f.accept(b);
    auto first=f.issue(ca);f.reads.finish_read(first.id);
    check(f.capacity.logical_reads().size()==1 && f.write().admission==SubmitResult::Backpressured,
          "first reader cannot clear another reader dependency");
    f.reads.finish_read(f.issue(cb).id);
    check(f.capacity.logical_reads().size()==0 && f.write().admission==SubmitResult::Accepted,
          "last content result frees rewrite before both responses deliver");
}
void pending_fixed_missing() {
    F f;f.write(0,0,1,30);
    const auto c=f.cmd(Op::Read,0,1);
    check(f.reads.submit(c).immediate_status==ReadStatus::PendingWriteMissing,"fix missing observation at acceptance");
    f.write(0,1,1,44);
    check(f.response(c.id).status==ReadStatus::PendingWriteMissing && !f.response(c.id).data,
          "later fragment cannot turn accepted 0xA into success");
}
void physical_zone_wakeup() {
    F f;auto lease=f.capacity.pin(f.address());int n=0;
    const auto key=f.capacity.zone_wait(f.address());
    f.signals.subscribe({f.signals.observe(key)},[&]{++n;});
    lease.reset();f.capacity.publish_releases();f.events.run();
    check(n==1,"zone remap drain sees physical lease release");
}
void fixture_capabilities() {
    const auto d=demonstration_profile();check(!d.capabilities().read_burst,"demo does not advertise unimplemented burst read");
    F f;auto c=f.cmd(Op::Write);c.read_group=5;
    rejects<std::invalid_argument>([&]{f.writes.submit(c,{DataToken{1}});});
    check(f.admission.total_outstanding()==0,"write cannot silently acquire a read-only ordering field");
}


const std::map<std::string,void(*)()> tests={
    {"issue_rollback",issue_rollback},{"issue_sampling",issue_sampling},{"guard_multiple_readers",guard_multiple_readers},
    {"pending_fixed_missing",pending_fixed_missing},{"physical_zone_wakeup",physical_zone_wakeup},{"fixture_capabilities",fixture_capabilities},
    {"event_rounds",event_rounds},{"deadline_batch",deadline_batch},{"deadline_limits",deadline_limits},
    {"deadline_exception",deadline_exception},{"event_cancel_limits",event_cancel_limits},
    {"wait_targeting",wait_targeting},{"wait_race_cancel",wait_race_cancel},{"wait_reregister",wait_reregister},
    {"read_accept_issue",read_accept_issue},{"read_bounded_results",read_bounded_results},
    {"read_order",read_order},{"read_order_errors",read_order_errors},{"pending_order_diagnostic",pending_order_diagnostic},
    {"pending_capacity",pending_capacity},{"read_failure_revalidation",read_failure_revalidation},
    {"read_repair_revalidation",read_repair_revalidation},{"read_quiesce_drain",read_quiesce_drain},
    {"read_stale_candidate",read_stale_candidate},{"write_probes",write_probes},
    {"program_stale_candidate",program_stale_candidate},{"write_ready_notifications",write_ready_notifications},
    {"physical_release_notification",physical_release_notification},{"deadline_credit_chain",deadline_credit_chain},
    {"deadline_resident_only",deadline_resident_only},{"deadline_two_dlus",deadline_two_dlus},
    {"window_membership",window_membership},{"group_channel_isolation",group_channel_isolation},
    {"teardown_queued_read",teardown_queued_read}
};
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::invalid_argument("scenario required");
        tests.at(argv[1])();std::cout<<"PASS "<<argv[1]<<" ("<<checks<<" checks)\n";
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}

