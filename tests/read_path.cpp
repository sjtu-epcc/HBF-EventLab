#include "hbf_eventlab/runtime/read_path.hpp"
#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include "../examples/support/bounded_read_results.hpp"
#include <iostream>
#include <string>
#include <type_traits>

using namespace hbf_eventlab;
namespace {
std::size_t checks = 0;
void check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    bool threw = false; try { action(); } catch (const std::exception&) { threw = true; }
    check(threw, "expected API diagnostic");
}
StaticDeviceDescription profile(std::uint32_t pending, std::uint32_t spares, std::uint32_t mocs) {
    Geometry g{2,2,2,8,3,spares};
    return {g, {256,pending,true,true}, {mocs,Tick{100},g.block_group_dlus()}};
}
struct Fixture {
    StaticDeviceDescription d;
    ReferenceAddressLayout layout;
    ZoneMapping zones;
    BlockReplacement replacement;
    NandMediaState media;
    CapacityRetirementCoordinator capacity;
    EventQueue events;
    AdmissionWaitRegistry signals{events};
    CommandAdmission admission;
    WritePath writes;
    test_support::BoundedReadResults results{256};
    ReadPath reads;
    LogicalBlockKey b{0,0,0,0};
    std::uint64_t serial = 0;
    explicit Fixture(std::uint32_t pending=8, std::uint32_t spares=2, std::uint32_t mocs=256)
        : d(profile(pending,spares,mocs)), layout(d.geometry()), zones(d), replacement(d.geometry()),
          media(d.geometry()), capacity(d,layout,zones,replacement,media),
          admission(d.geometry().host_channels,d.configuration().outstanding_limit,signals),
          writes(d,layout,capacity,media,events,admission), reads(d,layout,capacity,media,admission,writes,results) {}
    struct Issued { SubmitResult admission; ReadDisposition disposition; std::optional<ReadStatus> immediate_status; std::optional<ReadOperationId> operation; };
    Issued submit_and_issue(const HostCommand& cmd) {
        const auto accepted=reads.submit(cmd);
        std::optional<ReadOperationId> op;
        if (accepted.candidate) { const auto issued=reads.issue(*accepted.candidate); if(issued) op=issued->id; }
        return {accepted.admission,accepted.disposition,accepted.immediate_status,op};
    }
    ChannelAddress address(PageId page, std::uint32_t slot=0) const {
        return {b.channel,layout.encode_logical({b,page,slot*host_unit_bytes})};
    }
    DluKey key(PageId page) const { return {b.channel,LocalDlu{address(page).local.value/dlu_bytes}}; }
    HostCommand command(Op op, PageId page=0, std::uint32_t slot=0, std::uint32_t bytes=64) {
        return {CommandId{++serial},op,address(page,slot),bytes,0};
    }
    WriteSubmission write(PageId page, std::uint32_t slot=0, std::uint32_t count=64, std::uint64_t token=10) {
        return writes.submit(command(Op::Write,page,slot,count*64),std::vector<DataToken>(count,DataToken{token}));
    }
    void good(PageId page, std::uint64_t token=10) {
        const auto s=write(page,0,64,token);
        check(s.admission==SubmitResult::Accepted && !s.immediate_status,"legal write accepted");
    }
    void erase() { for (const auto& op:writes.pending_erases()) writes.finish_erase(writes.issue_erase(op.id)->id); }
    ProgramOperation start(PageId page) {
        const auto op=writes.issue_program(*writes.program_candidate(key(page))); check(op.has_value(),"program starts"); return *op;
    }
    void finish(PageId page, ProgramOutcome outcome=ProgramOutcome::Success) { writes.finish_program(start(page).id,outcome); }
    void deliver_writes() { for (const auto& r:writes.responses()) writes.response_delivered(r.channel,r.command); }
    void deliver_reads() { for (const auto& r:reads.responses()) reads.response_delivered(r.channel,r.command); }
    void committed(PageId page, std::uint64_t token=10) { good(page,token); erase(); finish(page); deliver_writes(); }
    ReadResponse response(CommandId id) const {
        for (const auto& r:reads.responses()) if (r.command==id) return r;
        throw std::runtime_error("read response missing");
    }
    ReadResponse immediate(PageId page, ReadStatus expected, std::uint32_t slot=0) {
        const auto cmd=command(Op::Read,page,slot); const auto s=reads.submit(cmd);
        check(s.admission==SubmitResult::Accepted && s.immediate_status==expected && !s.candidate,"immediate read status");
        const auto result=response(cmd.id); reads.response_delivered(cmd.address.channel,cmd.id); return result;
    }
    void swap() {
        capacity.declare_discardable(0,0); capacity.declare_discardable(0,1); capacity.quiesce_pair(0,0,1);
        check(capacity.commit_swap(0,0,1,[]{})==RemapResult::Applied,"discarded contents remap without old replay");
    }
};
void basic_errors() {
    Fixture f;
    auto bad=f.command(Op::Write); bad.address.local=LocalByteAddress{f.d.geometry().channel_bytes()};
    check(f.writes.submit(bad,{}).immediate_status==WriteStatus::InvalidAddress,"write invalid address completes 1");
    check(f.writes.pending(0)==0 && f.writes.pending_erases().empty(),"address error creates no DLU or erase");
    check(f.admission.outstanding(0)==1 && f.writes.responses().size()==1,"error holds one credit until delivery");
    f.deliver_writes();
    bad=f.command(Op::Read); bad.address.local=LocalByteAddress{UINT64_MAX};
    check(f.submit_and_issue(bad).immediate_status==ReadStatus::InvalidAddress,"read address overflow classified safely");
    check(!f.response(bad.id).data && f.reads.pending_reads().empty(),"invalid read no data or media work");
    f.deliver_reads();
    bad=f.command(Op::Write); bad.address.local=LocalByteAddress{UINT64_MAX}; bad.read_hint=ReadHint::Batch;
    check(f.writes.submit(bad,{}).immediate_status==WriteStatus::InvalidUserField,"modeled user field precedes address");
    f.deliver_writes();
    bad=f.command(Op::Read); bad.read_hint=static_cast<ReadHint>(99);
    check(f.submit_and_issue(bad).immediate_status==ReadStatus::InvalidUserField,"invalid read hint completes 3"); f.deliver_reads();
    bad=f.command(Op::Read); bad.size_bytes=0; rejects([&]{f.submit_and_issue(bad);});
    bad=f.command(Op::Read); bad.address.local=LocalByteAddress{1}; rejects([&]{f.submit_and_issue(bad);});
    bad=f.command(Op::Read,0,0,128); rejects([&]{f.submit_and_issue(bad);});
    bad=f.command(Op::Write); rejects([&]{f.writes.submit(bad,{});});
    check(f.admission.outstanding(0)==0 && f.writes.pending(0)==0,"API diagnostics consume no credit");
}
void shared_credit() {
    Fixture f;
    const auto first=f.command(Op::Write); f.writes.submit(first,{DataToken{7}});
    auto missing=f.command(Op::Read,0,1);
    check(f.submit_and_issue(missing).immediate_status==ReadStatus::PendingWriteMissing,"read shares write credit");
    const auto limit=f.d.configuration().outstanding_limit;
    for(std::uint32_t i=2;i<limit;++i)
        check(f.submit_and_issue(f.command(Op::Read,0,1)).immediate_status==ReadStatus::PendingWriteMissing,
              "accepted error responses fill the configured shared ledger");
    auto bad=f.command(Op::Write); bad.read_hint=static_cast<ReadHint>(99); bad.size_bytes=0;
    check(f.writes.submit(bad,{}).admission==SubmitResult::Backpressured,"full credit suppresses write validation");
    bad=f.command(Op::Read); bad.address.local=LocalByteAddress{UINT64_MAX}; bad.size_bytes=0;
    check(f.submit_and_issue(bad).admission==SubmitResult::Backpressured,"full credit suppresses read validation");
    check(f.admission.outstanding(0)==limit && f.writes.pending(0)==1 && f.reads.responses().size()==limit-1,"backpressure has no side effects");
    auto duplicate=first; duplicate.op=Op::Read; rejects([&]{f.submit_and_issue(duplicate);});
    f.b.channel=1; const auto other=f.command(Op::Read);
    const auto other_s=f.submit_and_issue(other); check(other_s.operation.has_value(),"channel credit independent");
    f.reads.finish_read(*other_s.operation); f.reads.response_delivered(1,other.id); f.b.channel=0;
    f.reads.response_delivered(0,missing.id);
    check(f.admission.outstanding(0)==limit-1,"one delivered error returns exactly one credit");
    f.immediate(0,ReadStatus::Success);
    check(f.admission.outstanding(0)==limit-1,"forwarded read uses and returns the last available credit");
    f.deliver_reads();
    check(f.writes.outstanding(0)==1 && f.reads.outstanding(0)==1,"both views use one ledger");
    f.immediate(0,ReadStatus::Success);
    CommandAdmission different(2,limit,f.signals);
    rejects([&]{ReadPath wrong(f.d,f.layout,f.capacity,f.media,different,f.writes,f.results);});
    check(!std::is_copy_constructible<CommandAdmission>::value,"ledger cannot be copied");
}
void pending_forward() {
    Fixture f(1);
    f.write(0,7,1,77);
    check(f.immediate(0,ReadStatus::Success,7).data==DataToken{77},"collecting data forwards before erase");
    check(!f.immediate(0,ReadStatus::PendingWriteMissing,8).data,"missing fragment never reads old NAND");
    check(f.write(0,7,1,99).immediate_status==WriteStatus::OverlapAddress,"duplicate write rejected alone");
    f.deliver_writes();
    for (std::uint32_t i=0;i<64;++i) if(i!=7)
        check(f.write(0,i,1,100+i).disposition==WriteDisposition::Supplement,"quota full still admits supplements");
    check(f.immediate(0,ReadStatus::Success,63).data==DataToken{163},"Ready context retains data");
    f.erase(); const auto op=f.start(0);
    check(f.immediate(0,ReadStatus::Success,7).data==DataToken{77},"Programming context forwards original data");
    f.writes.finish_program(op.id); f.deliver_writes();
    check(f.writes.pending(0)==0,"program completion releases Pending DLU");
    const auto cmd=f.command(Op::Read,0,7); const auto s=f.submit_and_issue(cmd);
    check(s.disposition==ReadDisposition::Queued && s.operation.has_value(),"committed read issues media ticket");
    f.reads.finish_read(*s.operation); check(f.response(cmd.id).data==DataToken{77},"NAND token matches forwarding");
    f.deliver_reads();
}
void media_lifetime() {
    Fixture f; f.committed(0,33);
    const auto cmd=f.command(Op::Read); const auto s=f.submit_and_issue(cmd);
    check(s.operation.has_value() && f.reads.responses().empty(),"media read waits for completion");
    const auto physical=block_of(f.capacity.inspect(f.address(0)));
    check(f.capacity.in_flight(physical)>0,"active read pins physical target");
    rejects([&]{f.reads.response_delivered(0,cmd.id);});
    check(f.write(0).admission==SubmitResult::Backpressured,"read lease blocks new erase");
    f.capacity.declare_discardable(0,0); f.capacity.declare_discardable(0,1); f.capacity.quiesce_pair(0,0,1);
    check(f.capacity.commit_swap(0,0,1,[]{})==RemapResult::InFlight,"read lease blocks mapping commit");
    f.capacity.cancel_quiesce(0,0,1);
    f.reads.finish_read(*s.operation);
    check(f.capacity.in_flight(physical)==0 && f.admission.outstanding(0)==1,"completion releases lease but holds credit");
    f.committed(0,44);
    check(f.response(cmd.id).data==DataToken{33},"ready response survives later overwrite");
    rejects([&]{f.reads.finish_read(*s.operation);});
    f.reads.response_delivered(0,cmd.id); rejects([&]{f.reads.response_delivered(0,cmd.id);});
    const auto blank=f.command(Op::Read,1); const auto blank_s=f.submit_and_issue(blank);
    f.reads.finish_read(*blank_s.operation);
    check(f.response(blank.id).status==ReadStatus::UnwrittenPage && !f.response(blank.id).data,"blank media gives 7 without data");
    f.deliver_reads();
}
void recovery_reads() {
    Fixture f; f.committed(0); f.committed(1); f.good(2); f.finish(2,ProgramOutcome::FailedReusable); f.deliver_writes();
    check(f.immediate(0,ReadStatus::TemporarilyRestricted).restriction==ReadRestrictionReason::AwaitingReplay,"failure restricts old prefix");
    check(f.reads.pending_reads().empty(),"recovery response does not wait on Host or pin media");
    f.write(0,2,1,72);
    check(f.immediate(0,ReadStatus::Success,2).data==DataToken{72},"replay Pending present overrides restriction");
    f.immediate(0,ReadStatus::PendingWriteMissing,3);
    check(f.immediate(1,ReadStatus::TemporarilyRestricted).restriction==ReadRestrictionReason::ReplayInProgress,"other pages stay restricted");
    f.erase();
    for (std::uint32_t i=0;i<64;++i) if(i!=2) f.write(0,i,1,72);
    f.finish(0); f.deliver_writes();
    f.immediate(0,ReadStatus::TemporarilyRestricted);
    f.committed(1); f.immediate(0,ReadStatus::TemporarilyRestricted);
    f.good(2); f.finish(2);
    const auto cmd=f.command(Op::Read); const auto s=f.submit_and_issue(cmd);
    check(s.operation.has_value(),"successful endpoint clears gate before write response delivery");
    f.reads.finish_read(*s.operation); f.deliver_reads(); f.deliver_writes();
}
void merged_read_gate() {
    Fixture f; for(PageId p=0;p<4;++p) f.committed(p);
    f.good(4); f.finish(4,ProgramOutcome::FailedReusable); f.deliver_writes();
    f.committed(0); f.good(1); f.finish(1,ProgramOutcome::FailedReusable); f.deliver_writes();
    check(f.writes.order(f.b).replay_through()==std::optional<PageId>{4},"earlier failure preserves far endpoint");
    f.immediate(0,ReadStatus::TemporarilyRestricted);
    for(PageId p=0;p<=4;++p) {
        f.committed(p,100+p);
        if(p<4) f.immediate(0,ReadStatus::TemporarilyRestricted);
    }
    const auto cmd=f.command(Op::Read,4); const auto s=f.submit_and_issue(cmd);
    check(s.operation.has_value(),"merged endpoint alone releases restriction");
    f.reads.finish_read(*s.operation); check(f.response(cmd.id).data==DataToken{104},"restored endpoint data visible"); f.deliver_reads();
}
void replacement_timeout_gate() {
    Fixture f(8,1); f.committed(0); f.committed(1);
    f.good(2); f.finish(2,ProgramOutcome::FailedUnusable); f.deliver_writes();
    f.immediate(0,ReadStatus::TemporarilyRestricted);
    f.committed(0);
    check(f.capacity.inspect(f.address(0)).block==3,"replacement redirects replay to spare");
    f.immediate(0,ReadStatus::TemporarilyRestricted);
    f.write(1,0,1); f.events.run();
    check(f.writes.responses().front().status==WriteStatus::AssemblyTimeout,"replay fragment times out"); f.deliver_writes();
    f.immediate(1,ReadStatus::TemporarilyRestricted);
    f.committed(1); f.immediate(0,ReadStatus::TemporarilyRestricted); f.committed(2);
    check(f.writes.read_view(f.b).replay==ReplayPhase::Idle,"replacement and timeout do not independently discharge debt");
}
void retirement_reads() {
    Fixture f(8,0);
    f.capacity.mark_unusable(block_of(f.capacity.inspect(f.address(0))));
    check(f.immediate(0,ReadStatus::TemporarilyRestricted).restriction==ReadRestrictionReason::PhysicalRepairRequired,
          "known failed target alone gives chosen 2, not ECC/capacity/die errors");
    check(f.write(0).immediate_status==WriteStatus::CapacityUnusable,"new start commits retirement and reports 8");
    f.immediate(0,ReadStatus::InvalidAddress);
    check(f.write(0).immediate_status==WriteStatus::InvalidAddress,"later write to retired address gives 1");
    check(f.writes.pending(0)==0 && f.writes.pending_erases().empty(),"retirement errors create no Pending DLU"); f.deliver_writes();
    check(f.admission.outstanding(0)==0,"all errors return shared credits on delivery");
}
void remap_read_generation() {
    Fixture f; f.committed(0,11); f.good(1); f.finish(1,ProgramOutcome::FailedReusable); f.deliver_writes();
    f.b.block=1; f.committed(0,88); f.b.block=0;
    f.swap();
    check(f.writes.read_view(f.b).replay==ReplayPhase::Idle && !f.writes.read_view(f.b).current_contents,"committed discard supersedes stale replay view");
    f.immediate(0,ReadStatus::UnwrittenPage);
    check(f.media.read(f.capacity.inspect(f.address(0)))->at(0)==DataToken{88},"remap did not erase or copy physical contents");
    f.swap(); f.immediate(0,ReadStatus::UnwrittenPage);
    f.good(0,99); f.erase(); f.finish(0,ProgramOutcome::FailedReusable); f.deliver_writes();
    check(f.writes.order(f.b).replay_through()==std::optional<PageId>{0},"new content failure never merges discarded old debt");
    f.immediate(0,ReadStatus::TemporarilyRestricted); f.committed(0,99);
    const auto cmd=f.command(Op::Read); const auto s=f.submit_and_issue(cmd); f.reads.finish_read(*s.operation);
    check(f.response(cmd.id).data==DataToken{99},"fresh rewrite is visible after ABA remap"); f.deliver_reads();
}
void event_error_order() {
    for (const auto tick : {100u,101u}) {
        Fixture f; f.good(0); f.erase(); const auto op=f.start(0); f.write(1,0,1);
        f.events.schedule_at(Tick{tick},EventPhase::Completion,[&]{f.writes.finish_program(op.id,ProgramOutcome::FailedReusable);});
        f.events.schedule_at(Tick{tick},EventPhase::Arrival,[&]{f.immediate(0,ReadStatus::TemporarilyRestricted);});
        f.events.run();
        const auto replies=f.writes.responses(); check(replies.size()==2,"each accepted parent completed once");
        check(replies[0].status==WriteStatus::ProgramFail,"failed DLU receives 7");
        check(replies[1].status==(tick==100?WriteStatus::WriteOrderViolation:WriteStatus::AssemblyTimeout),
              "same-tick failure beats deadline; earlier timeout remains final");
        if(tick==100) check(replies[1].additional_info==2,"failed predecessor invalidates accepted follower");
        f.deliver_writes();
    }
    Fixture f; f.good(0); f.erase(); f.finish(0,ProgramOutcome::FailedReusable); f.deliver_writes();
    f.good(0); f.erase(); const auto op=f.start(0);
    f.events.schedule_at(Tick{10},EventPhase::Completion,[&]{f.writes.finish_program(op.id);});
    f.events.schedule_at(Tick{10},EventPhase::Arrival,[&]{
        const auto cmd=f.command(Op::Read); const auto s=f.submit_and_issue(cmd);
        check(s.operation.has_value(),"arrival observes endpoint success at same tick"); f.reads.finish_read(*s.operation);
    });
    f.events.run(); f.deliver_reads(); f.deliver_writes();
}
void deadline_forward() {
    Fixture f; f.write(0,0,1,55); f.erase();
    CommandId present,missing;
    f.events.schedule_at(Tick{100},EventPhase::Arrival,[&]{
        auto cmd=f.command(Op::Read); present=cmd.id; f.submit_and_issue(cmd);
        cmd=f.command(Op::Read,0,1); missing=cmd.id; f.submit_and_issue(cmd);
        check(f.write(0,0,1).immediate_status==WriteStatus::OverlapAddress,"same-tick duplicate is a separate 2");
    });
    f.events.run();
    check(f.response(present).status==ReadStatus::Success && f.response(present).data==DataToken{55},"later timeout does not revoke forwarded result");
    check(f.response(missing).status==ReadStatus::PendingWriteMissing,"arrival before deadline sees Pending missing");
    const auto replies=f.writes.responses();
    check(replies.size()==2 && replies[0].status==WriteStatus::AssemblyTimeout && replies[1].status==WriteStatus::OverlapAddress,
          "timeout affects accepted original, not previously completed duplicate");
    f.deliver_reads(); f.deliver_writes();
    const auto cmd=f.command(Op::Read); const auto s=f.submit_and_issue(cmd);
    check(s.operation.has_value(),"ordinary assembly timeout creates no recovery restriction"); f.reads.finish_read(*s.operation);
    check(f.response(cmd.id).status==ReadStatus::UnwrittenPage,"timed-out data not committed"); f.deliver_reads();
}
void quiesce_reads() {
    Fixture f; f.write(0,0,1,60); f.erase();
    f.capacity.declare_discardable(0,0); f.capacity.declare_discardable(0,1); f.capacity.quiesce_pair(0,0,1);
    f.immediate(0,ReadStatus::Success); f.immediate(0,ReadStatus::PendingWriteMissing,1);
    const auto before=f.admission.outstanding(0);
    check(f.submit_and_issue(f.command(Op::Read,1)).admission==SubmitResult::Backpressured,"new media read waits for mapping drain");
    check(f.admission.outstanding(0)==before,"quiesce backpressure creates no accepted command");
    check(f.write(0,1,1).disposition==WriteDisposition::Supplement,"accepted context can continue during quiesce");
    f.capacity.cancel_quiesce(0,0,1);
    Fixture erase; erase.write(0,0,1);
    check(erase.submit_and_issue(erase.command(Op::Read,1)).admission==SubmitResult::Backpressured,"normal internal erase waits without status 2");
    erase.erase(); const auto cmd=erase.command(Op::Read,1); const auto s=erase.submit_and_issue(cmd);
    check(s.operation.has_value(),"internal erase completion permits media read"); erase.reads.finish_read(*s.operation); erase.deliver_reads();
}
void read_issue_snapshot() {
    Fixture f; f.committed(0,20);
    const auto cmd=f.command(Op::Read,1); const auto s=f.submit_and_issue(cmd);
    check(f.write(1).admission==SubmitResult::Backpressured,"earlier read guards unwritten page");
    f.reads.finish_read(*s.operation); f.committed(1,21);
    check(f.response(cmd.id).status==ReadStatus::UnwrittenPage,"issued read never sees future program data"); f.deliver_reads();
    const auto older=f.command(Op::Read); const auto old_s=f.submit_and_issue(older);
    f.good(2); f.finish(2,ProgramOutcome::FailedUnusable); f.deliver_writes();
    f.reads.finish_read(*old_s.operation);
    check(f.response(older.id).status==ReadStatus::Success && f.response(older.id).data==DataToken{20},"initial model retains already-issued result across later fault");
    f.deliver_reads(); f.immediate(0,ReadStatus::TemporarilyRestricted);
}
void ingress_scope() {
    Fixture f(1); f.write(0,0,1);
    for(int i=0;i<2;++i) {
        check(f.write(1,0,1).immediate_status==WriteStatus::WriteOrderViolation,"unassembled predecessor gives current-command 6");
        const auto responses=f.writes.responses(); check(responses.back().additional_info==1,"early next page uses info 1");
    }
    check(f.writes.pending(0)==1 && !f.writes.find(f.key(1)),"rejected next-page requests create no failed phantom DLU");
    f.deliver_writes();
    f.b.bank=1; check(f.write(0).immediate_status==WriteStatus::PendingDluLimit,"otherwise legal new DLU hits quota");
    check(f.writes.pending_erases().size()==1,"quota error does not start preparation"); f.deliver_writes(); f.b.bank=0;
    f.erase(); f.events.run();
    check(f.writes.responses().size()==1 && f.writes.responses()[0].status==WriteStatus::AssemblyTimeout,"only accepted original remains in timeout scope"); f.deliver_writes();
}
void read_teardown() {
    Fixture f; const auto cmd=f.command(Op::Read);
    {
        ReadPath temporary(f.d,f.layout,f.capacity,f.media,f.admission,f.writes,f.results);
        check(temporary.submit(cmd).candidate.has_value(),"temporary reader issues work");
        check(f.write(0).admission==SubmitResult::Backpressured,"teardown test has an active read lease");
    }
    check(f.admission.outstanding(0)==0 && f.capacity.in_flight(block_of(f.capacity.inspect(f.address(0))))==0,
          "teardown releases only its own command and lease");
    f.good(0);
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2) throw std::invalid_argument("scenario required"); const std::string s=argv[1];
        if(s=="basic_errors") basic_errors();
        else if(s=="shared_credit") shared_credit();
        else if(s=="pending_forward") pending_forward();
        else if(s=="media_lifetime") media_lifetime();
        else if(s=="recovery_reads") recovery_reads();
        else if(s=="merged_read_gate") merged_read_gate();
        else if(s=="replacement_timeout_gate") replacement_timeout_gate();
        else if(s=="retirement_reads") retirement_reads();
        else if(s=="remap_read_generation") remap_read_generation();
        else if(s=="event_error_order") event_error_order();
        else if(s=="deadline_forward") deadline_forward();
        else if(s=="quiesce_reads") quiesce_reads();
        else if(s=="read_issue_snapshot") read_issue_snapshot();
        else if(s=="ingress_scope") ingress_scope();
        else if(s=="read_teardown") read_teardown();
        else throw std::invalid_argument("unknown scenario");
        std::cout << "PASS " << s << " (" << checks << " checks)\n";
    } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
