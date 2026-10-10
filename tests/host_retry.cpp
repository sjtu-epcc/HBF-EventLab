#include "hbf_eventlab/runtime/simulation_runner.hpp"
#include "hbf_eventlab/policies/round_robin_interleaver.hpp"
#include <algorithm>
#include <iostream>
using namespace hbf_eventlab;
namespace {
std::size_t checks=0;
void check(bool value,const char* message) {++checks;if(!value)throw std::runtime_error(message);}
StaticDeviceDescription profile(unsigned pending=8,Tick deadline=Tick{10000000}) {
    Geometry g{1,1,1,8,4,1};return {g,{256,pending,false,true},{256,deadline,g.block_group_dlus()}};
}
GlobalRequest write(std::uint64_t id,std::uint64_t address,std::uint64_t bytes=4096,Tick at={}) {
    GlobalRequest r{GlobalRequestId{id},Op::Write,GlobalByteAddress{address},bytes,1};r.not_before=at;return r;
}

// A public-port test double gives deterministic feedback races without exposing
// Device internals to Host. It rejects duplicate stored bytes and finishes only
// complete pages. Real Device integration tests below cover the same mechanisms.
class ScriptPort:public IHostPort {
public:
    struct Record {LocalTransaction tx;Receiver response;TransferAttemptId attempt;Tick handed;bool done=false;};
    struct Page {std::bitset<64> received;std::vector<CommandId> commands;bool complete=false;};
    EventQueue events;AdmissionWaitRegistry signals{events};
    StaticDeviceDescription desc=profile();ReferenceAddressLayout address_layout{desc.geometry()};
    std::map<CommandId,Record> records;
    std::map<std::uint64_t,Page> pages;
    std::map<std::uint64_t,unsigned> attempts;
    std::map<std::uint64_t,unsigned> next_page;
    std::function<void(CommandId,unsigned)> handler;
    std::uint64_t next=0;unsigned order_errors=0;
    const StaticDeviceDescription& description()const override{return desc;}
    const IAddressLayout& layout()const override{return address_layout;}
    AdmissionWaitRegistry& notifications()override{return signals;}
    HandoffResult try_submit(const LocalTransaction& tx,Receiver reply,AcceptanceObserver accepted)override {
        const TransferAttemptId attempt{++next};const auto id=tx.command.id;
        check(records.emplace(id,Record{tx,std::move(reply),attempt,events.now()}).second,"new command ID for every retry");
        const auto count=++attempts[tx.command.address.local.value];
        events.schedule_at(events.now(),EventPhase::Arrival,[this,id,attempt,accepted,count] {
            if(accepted)accepted({attempt,0,id,events.now()});
            if(handler)handler(id,count);else store(id);
        });
        return {HandoffDisposition::Taken,attempt,{}};
    }
    void after(std::uint64_t delay,std::function<void()> f) {
        events.schedule_at(Tick{events.now().value+delay},EventPhase::Arrival,std::move(f));
    }
    void reply(CommandId id,WriteStatus status,std::uint8_t info=0,std::uint64_t delay=1) {
        after(delay,[this,id,status,info] {
            auto& r=records.at(id);check(!r.done,"one final response per attempt");r.done=true;
            r.response({r.attempt,WriteResponse{0,id,status,info},events.now()});
        });
    }
    void store(CommandId id,std::uint64_t order_delay=1) {
        const auto& tx=records.at(id).tx;const auto address=tx.command.address.local.value;
        const auto logical=address/4096;auto& page=pages[logical];
        if(logical%8!=next_page[logical/8]) {++order_errors;reply(id,WriteStatus::WriteOrderViolation,1,order_delay);return;}
        for(std::size_t i=0;i<tx.data.size();++i) {
            const auto slot=address%4096/64+i;
            check(!page.received.test(slot),"retry must not duplicate valid Assembly data");
            check(tx.data[i]==token_for(1,GlobalByteAddress{address+i*64}),"retry retains immutable Host data");
            page.received.set(slot);
        }
        page.commands.push_back(id);
        if(page.received.all()) {
            page.complete=true;++next_page[logical/8];
            for(auto command:page.commands)reply(command,WriteStatus::Success,0,20);
            page.commands.clear();
        }
    }
    void expire(std::uint64_t page) {
        auto& p=pages[page];if(p.complete)return;
        for(auto id:p.commands)reply(id,WriteStatus::AssemblyTimeout);
        p=Page{};
    }
};
struct ScriptFixture {
    ScriptPort port;RoundRobinInterleaver inter{GlobalByteAddress{},1,port.desc.geometry().channel_bytes()};
    HostDriver host;std::vector<std::string> actions;
    explicit ScriptFixture(HostConfiguration c={}):host(port,port.events,inter,c) {
        host.set_trace([this](const HostTrace& e){actions.push_back(e.action);});
    }
    void run(std::vector<GlobalRequest> input,bool success=true) {
        host.load(input);host.start();port.events.set_limits({200000,10000,10000});port.events.run();
        check(host.activity().drained(),"Host drains retry records, timers and attempts");
        check(host.workload_success()==success,"logical outcome");
        check(host.statistics().handed==host.statistics().responses,"all handed attempts settled");
        for(const auto& r:port.records)check(r.second.done,"no lost old attempt");
    }
};
void pending_followers(bool follower_first,bool late) {
    HostConfiguration c;c.max_open_dlus_per_channel=4;c.pending_retry_delay=Tick{10};ScriptFixture f(c);
    f.port.handler=[&](CommandId id,unsigned attempt) {
        const auto address=f.port.records.at(id).tx.command.address.local.value;
        if(address==0 && attempt==1) {f.port.reply(id,WriteStatus::PendingDluLimit,0,100);return;}
        if(late && address>=4096+32*64 && address<8192 && attempt==1) {
            f.port.after(300,[&,id]{f.port.store(id);});return;
        }
        f.port.store(id,follower_first?10:200);
    };
    f.run({write(1,0,8192)});
    check(f.host.statistics().pending_retry_commands==1,"only refused fragment is retried");
    check(f.host.statistics().order_retry_commands==(late?32:64),"only rejected followers retried");
    check(f.host.statistics().recovery_tasks==0,"resource retry is not block replay");
    if(follower_first)check(std::find(f.actions.begin(),f.actions.end(),"order-wait")<
        std::find(f.actions.begin(),f.actions.end(),"pending-limit"),"order response really precedes root");
    for(unsigned slot=1;slot<64;++slot)check(f.port.attempts[slot*64]==1,"valid predecessor fragments preserved");
    if(late)for(unsigned slot=32;slot<64;++slot)check(f.port.attempts[4096+slot*64]==1,"late valid followers preserved");
}
void pending_root_first(){pending_followers(false,false);}
void pending_follower_first(){pending_followers(true,false);}
void pending_late_followers(){pending_followers(true,true);}
void pending_budget(bool disabled) {
    HostConfiguration c;c.max_pending_retries=disabled?0:2;c.pending_retry_delay=Tick{7};ScriptFixture f(c);
    f.port.handler=[&](CommandId id,unsigned) {
        if(f.port.records.at(id).tx.command.address.local.value==0)f.port.reply(id,WriteStatus::PendingDluLimit);
        else f.port.store(id);
    };
    f.port.after(1000,[&]{f.port.expire(0);});f.run({write(1,0)},false);
    check(f.port.attempts[0]==(disabled?1:3),"bounded additional attempts, independent of 64 responses");
    check(f.host.results().at(GlobalRequestId{1}).reason==(disabled?"WriteStatus:4":"PendingRetryBudgetExhausted"),"specific failure");
}
void pending_exhausted(){pending_budget(false);}
void pending_disabled(){pending_budget(true);}
void pending_progress() {
    HostConfiguration c;c.max_open_dlus_per_channel=4;c.pending_retry_delay=Tick{1000};ScriptFixture f(c);
    f.port.handler=[&](CommandId id,unsigned n) {
        if(f.port.records.at(id).tx.command.address.local.value==0 && n==1)f.port.reply(id,WriteStatus::PendingDluLimit,0,100);
        else f.port.store(id);
    };
    f.run({write(1,0),write(2,32768)});
    for(const auto& r:f.port.records)if(r.second.tx.command.address.local.value==0 && r.second.handed.value)
        check(r.second.handed.value==100,"capacity feedback before delayed error avoids arbitrary sleep");
    check(f.port.events.now().value<1000,"obsolete retry timer canceled");
}
void pending_burst() {
    HostConfiguration c;c.writes=WriteCommandMode::Burst4096;c.pending_retry_delay=Tick{5};ScriptFixture f(c);
    f.port.handler=[&](CommandId id,unsigned n) {if(n==1)f.port.reply(id,WriteStatus::PendingDluLimit);else f.port.store(id);};
    f.run({write(1,0)});check(f.host.statistics().handed==2,"one failed burst, one successful burst");
}
void timeout_late_fragments() {
    ScriptFixture f;
    f.port.handler=[&](CommandId id,unsigned n) {
        if(n==1 && f.port.records.at(id).tx.command.address.local.value>=2048)
            f.port.after(50,[&,id]{f.port.store(id);});
        else f.port.store(id);
    };
    f.port.after(20,[&]{f.port.expire(0);});f.port.after(80,[&]{f.port.expire(0);});
    f.run({write(1,0)});
    check(f.host.statistics().timeout_retry_rounds==1 && f.host.statistics().timeout_retry_commands==64,"one merged full-page retry");
    check(f.host.statistics().assembly_timeout_errors==64,"all old timeout responses counted, one retry round");
    unsigned retried=0;for(const auto& r:f.port.records)if(r.second.handed.value) {
        ++retried;check(r.second.handed.value>=81,"no new assembly until late old attempts settle");
    }
    check(retried==64,"all 64 fragments resent exactly once");
}
void timeout_future_data(bool missing) {
    HostConfiguration c;if(missing)c.retained_fragments=0;ScriptFixture f(c);
    f.port.after(20,[&]{f.port.expire(0);});
    f.run({write(1,0,2048),write(2,2048,2048,Tick{100})},!missing);
    if(missing)check(f.host.results().at(GlobalRequestId{1}).reason=="WriteStatus:5 MissingRetryData","no secret retained payload");
    else {
        check(f.host.statistics().timeout_retry_rounds==1 && f.host.statistics().timeout_retry_commands==32,"future source data not counted as retransmission");
        for(const auto& r:f.port.records)if(r.second.handed.value)check(r.second.handed.value>=100,"wait for all source data before opening retry");
    }
}
void timeout_release(){timeout_future_data(false);}
void timeout_missing_retention(){timeout_future_data(true);}
void timeout_missing_coverage() {
    ScriptFixture f;f.port.after(20,[&]{f.port.expire(0);});f.run({write(1,0,64)},false);
    check(f.host.statistics().timeout_retry_rounds==0,"cannot invent absent workload data");
}
void unexplained_order() {
    ScriptFixture f;f.run({write(1,0)}); // normal control for port semantics
    ScriptFixture g;g.port.handler=[&](CommandId id,unsigned){g.port.reply(id,WriteStatus::WriteOrderViolation,1);};
    g.run({write(1,0)},false);
    check(g.host.results().at(GlobalRequestId{1}).reason=="UnexplainedWriteOrderViolation","not every 0x6 is retryable");
}

void retry_during_replay(bool timeout) {
    HostConfiguration c;c.pending_retry_delay=Tick{5};ScriptFixture f(c);
    std::vector<CommandId> original;
    f.port.handler=[&](CommandId id,unsigned attempt) {
        const auto address=f.port.records.at(id).tx.command.address.local.value;
        if(attempt==1) {
            original.push_back(id);
            if(original.size()==64)for(auto old:original)f.port.reply(old,WriteStatus::ProgramFail);
            return;
        }
        if(attempt==2 && !address) {
            if(!timeout) {f.port.reply(id,WriteStatus::PendingDluLimit,0,50);return;}
            f.port.after(20,[&]{f.port.expire(0);});
            f.port.after(80,[&]{f.port.expire(0);});
        }
        if(timeout && attempt==2 && address>=2048)f.port.after(50,[&,id]{f.port.store(id);});
        else f.port.store(id);
    };
    f.run({write(1,0)});
    check(f.host.statistics().recovery_tasks==1 && f.host.statistics().replay_passes==1,
          "resource retries do not start another program-fail replay pass");
    check(f.host.recovery().tasks().begin()->second.phase==RecoveryPhase::Completed,"original replay completes");
    if(timeout)check(f.host.statistics().timeout_retry_rounds==1 && f.host.statistics().timeout_retry_commands==64,
                     "timeout inside replay resends the full page after drain");
    else check(f.host.statistics().pending_retry_commands==1,"Pending-full inside replay retries only refused command");
}
void pending_during_replay(){retry_during_replay(false);}
void timeout_during_replay(){retry_during_replay(true);}

void real_pending() {
    EventQueue events;ExecutionConfiguration ec;ec.program=Tick{3000000};
    DeviceDriver device(profile(1),ec,events);RoundRobinInterleaver inter(GlobalByteAddress{},1,device.description().geometry().channel_bytes());
    HostConfiguration hc;hc.max_open_dlus_per_channel=3;hc.pending_retry_delay=Tick{10000000};
    HostDriver host(device,events,inter,hc);SimulationRunner runner(events,host,device);
    host.load({write(1,0),write(2,32768)});host.start();auto result=runner.run();
    check(result.status==RunStatus::Completed && result.workload_success,"real Pending-full workload recovers");
    check(result.host.drained()&&result.device.drained(),"real loop fully drains");
    check(host.statistics().pending_retry_commands>0 && device.statistics().programs==2,"real resource refusal then two programs");
}
void real_timeout(bool persistent) {
    EventQueue events;ExecutionConfiguration ec;auto desc=profile(8,Tick{persistent?50000ULL:300000ULL});
    if(persistent)desc=StaticDeviceDescription{desc.geometry(),{256,8,false,true},{1,Tick{50000},desc.geometry().block_group_dlus()}};
    DeviceDriver device(desc,ec,events);RoundRobinInterleaver inter(GlobalByteAddress{},1,desc.geometry().channel_bytes());
    HostConfiguration hc;hc.max_timeout_retries=2;HostDriver host(device,events,inter,hc);SimulationRunner runner(events,host,device);
    if(persistent)host.load({write(1,0)});
    else host.load({write(1,0,1024),write(2,1024,3072,Tick{1000000})});
    host.start();auto result=runner.run();
    check(result.status==RunStatus::Completed && result.host.drained() && result.device.drained(),"real timeout loop drains");
    if(persistent) {
        check(!result.workload_success && host.statistics().timeout_retry_rounds==2,"impossible MOCS bounded retries");
        check(host.results().at(GlobalRequestId{1}).reason=="AssemblyRetryBudgetExhausted","distinct timeout budget");
    } else check(result.workload_success && host.statistics().timeout_retry_rounds>0,"real timeout retry succeeds");
}
void real_timeout_success(){real_timeout(false);}
void real_timeout_budget(){real_timeout(true);}
}
int main(int argc,char** argv) {
    const std::map<std::string,void(*)()> tests{
        {"pending_root_first",pending_root_first},{"pending_follower_first",pending_follower_first},
        {"pending_late_followers",pending_late_followers},{"pending_exhausted",pending_exhausted},
        {"pending_disabled",pending_disabled},{"pending_progress",pending_progress},{"pending_burst",pending_burst},
        {"timeout_late_fragments",timeout_late_fragments},{"timeout_release",timeout_release},
        {"timeout_missing_retention",timeout_missing_retention},{"timeout_missing_coverage",timeout_missing_coverage},
        {"pending_during_replay",pending_during_replay},{"timeout_during_replay",timeout_during_replay},
        {"unexplained_order",unexplained_order},{"real_pending",real_pending},
        {"real_timeout_success",real_timeout_success},{"real_timeout_budget",real_timeout_budget}};
    try {
        if(argc!=2 || !tests.count(argv[1]))throw std::invalid_argument("unknown test");
        tests.at(argv[1])();std::cout<<"PASS "<<argv[1]<<" ("<<checks<<" checks)\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<(argc>1?argv[1]:"")<<": "<<e.what()<<"\n";return 1;}
}
