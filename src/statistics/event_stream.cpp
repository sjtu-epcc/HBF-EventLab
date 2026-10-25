#include "hbf_eventlab/statistics/event_stream.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

namespace hbf_eventlab {
namespace {
std::string quoted(const std::string& text) {
    std::ostringstream out;out.imbue(std::locale::classic());out<<'"';
    for(unsigned char c:text) {
        if(c=='"'||c=='\\')out<<'\\'<<c;
        else if(c<32)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;
        else out<<c;
    }
    out<<'"';return out.str();
}
// Every event is serialized by value. References across lines use stable IDs;
// a flush is never an artificial completion or a new DLU generation.
struct Record {
    std::ostringstream out;
    explicit Record(std::uint64_t seq) {out.imbue(std::locale::classic());out<<"{\"schema_version\":1,\"sequence\":"<<seq;}
    void type(const char* s){out<<",\"type\":"<<quoted(s);}
    void field(const char* key,std::uint64_t value){out<<','<<quoted(key)<<':'<<value;}
    void flag(const char* key,bool value){out<<','<<quoted(key)<<':'<<(value?"true":"false");}
    void text(const char* key,const std::string& value){out<<','<<quoted(key)<<':'<<quoted(value);}
    void at(Tick value){field("at",value.value);}
    template<class T>void optional_id(const char* key,std::optional<T> value){out<<','<<quoted(key)<<':';if(value)out<<value->value;else out<<"null";}
    template<class T>void optional_number(const char* key,std::optional<T> value){out<<','<<quoted(key)<<':';if(value)out<<static_cast<std::uint64_t>(*value);else out<<"null";}
    template<class T>void ids(const char* key,const std::vector<T>& values){out<<','<<quoted(key)<<":[";bool comma=false;for(auto v:values){if(comma)out<<',';comma=true;out<<v.value;}out<<']';}
    void write(const StatGlobalDefined& e){type("GlobalDefined");field("id",e.id.value);field("op",unsigned(e.op));field("bytes",e.bytes);field("release",e.release.value);}
    void write(const StatInputRegistered& e){type("InputRegistered");field("id",e.id.value);field("offered",e.offered.value);field("registered",e.registered.value);}
    void write(const StatWorkDefined& e){type("WorkDefined");field("id",e.id.value);field("parent",e.parent.value);field("channel",e.address.channel);field("local_byte",e.address.local.value);optional_number("content",e.content);}
    void write(const StatGlobalReleased& e){type("GlobalReleased");field("id",e.id.value);at(e.at);}
    void write(const StatWorkFinished& e){type("WorkFinished");field("id",e.id.value);at(e.at);flag("success",e.success);text("reason",e.reason);}
    void write(const StatGlobalFinished& e){type("GlobalFinished");field("id",e.id.value);at(e.at);flag("success",e.success);text("reason",e.reason);}
    void write(const StatAttemptStarted& e){
        type("AttemptStarted");field("id",e.id.value);at(e.at);const auto& c=e.command;
        field("command",c.id.value);field("op",unsigned(c.op));field("channel",c.address.channel);field("local_byte",c.address.local.value);
        field("bytes",c.size_bytes);field("axi_id",c.axi_id);field("read_hint",unsigned(c.read_hint));optional_number("read_group",c.read_group);
    }
    void write(const StatAttemptLinked& e){type("AttemptLinked");field("id",e.id.value);field("unit",e.unit.value);field("queued_at",e.queued_at.value);field("unit_attempt",e.unit_attempt);flag("replay",e.replay);field("replay_pass",e.replay_pass);optional_number("content",e.content);ids("work",e.work);}
    void write(const StatAttemptAdvanced& e){type("AttemptAdvanced");field("id",e.id.value);field("stage",unsigned(e.stage));at(e.at);}
    void write(const StatAttemptDelivered& e){type("AttemptDelivered");field("id",e.id.value);at(e.at);field("op",unsigned(e.op));field("status",e.status);}
    void write(const StatAttemptAssessed& e){type("AttemptAssessed");field("id",e.id.value);at(e.at);flag("success",e.success);}
    void write(const StatPendingInitialized& e){type("PendingInitialized");field("channel",e.channel);field("limit",e.limit);at(e.at);}
    void write(const StatDluObserved& e){
        type("DluObserved");at(e.at);const auto& d=e.observation;
        field("change",unsigned(d.change));field("channel",d.key.channel);field("dlu",d.key.dlu.value);field("generation",d.generation.value);
        field("phase",unsigned(d.phase));field("received_fragments",d.received_fragments);field("first_arrival",d.first_arrival.value);field("deadline",d.deadline.value);
        optional_id("command",d.command);optional_number("status",d.status);field("additional_info",d.additional_info);
    }
    void write(const StatBankOperationStarted& e){
        type("BankOperationStarted");at(e.at);field("ticket",e.ticket);field("kind",unsigned(e.kind));const auto& a=e.target;
        field("channel",a.channel);field("die",a.die);field("bank",a.bank);field("block",a.block);field("page",a.page);field("byte_offset",a.byte_offset);
    }
    void write(const StatBankOperationFinished& e){type("BankOperationFinished");at(e.at);field("ticket",e.ticket);}
    void write(const StatReadWindowEntered& e){type("ReadWindowEntered");at(e.at);field("id",e.id.value);}
    void write(const StatReadWindowRemoved& e){type("ReadWindowRemoved");at(e.at);field("id",e.id.value);}
    void write(const StatReadWindowReleased& e){type("ReadWindowReleased");at(e.at);field("channel",e.channel);ids("members",e.members);field("reason",unsigned(e.reason));field("first_arrival",e.first_arrival.value);field("deadline",e.deadline.value);}
    void write(const StatReadServed& e){type("ReadServed");at(e.at);field("id",e.id.value);field("kind",unsigned(e.kind));optional_number("bank_ticket",e.bank_ticket);}
    std::string finish(){out<<"}\n";return out.str();}
};
}
StatisticsEventStream::StatisticsEventStream(std::ostream& out,std::size_t bytes):out_(out),limit_(bytes) {
    if(!bytes)throw std::invalid_argument("statistics event buffer must be positive");
    if(!out_)throw std::runtime_error("statistics event stream is not writable");
}
void StatisticsEventStream::write(const std::string& text) {
    if(failed_)throw std::runtime_error("statistics event stream previously failed");
    try {out_.write(text.data(),static_cast<std::streamsize>(text.size()));if(!out_)throw std::runtime_error("statistics event stream write failed");}
    catch(...) {failed_=true;throw;}
}
void StatisticsEventStream::append(std::string record) {
    // An individual record may exceed the buffer limit (e.g. a large read set).
    // Serialize it once and write directly; do not truncate or split the JSON.
    if(record.size()>limit_-buffer_.size())flush();
    if(record.size()>=limit_) {write(record);flush();return;}
    buffer_+=record;peak_=std::max(peak_,buffer_.size());
    if(buffer_.size()==limit_)flush();
}
void StatisticsEventStream::observe(const StatisticsEvent& e) {
    if(closed_||failed_)throw std::logic_error("statistics event stream is closed or failed");
    const auto next=checked_add(sequence_,1);Record record(next);
    std::visit([&](const auto& value){record.write(value);},e);
    append(record.finish());sequence_=next;
}
void StatisticsEventStream::flush() {
    if(failed_)throw std::runtime_error("statistics event stream previously failed");
    try {
        if(!buffer_.empty())write(buffer_);
        out_.flush();if(!out_)throw std::runtime_error("statistics event stream flush failed");
        buffer_.clear(); // discard only after a successful stream flush
    } catch(...) {failed_=true;throw;}
}
void StatisticsEventStream::finish(Tick at,const std::string& stop) {
    if(closed_)return;
    const auto next=checked_add(sequence_,1);Record record(next);record.type("StreamEnd");record.at(at);record.text("stop_reason",stop);
    append(record.finish());flush();sequence_=next;closed_=true;
}
} // namespace hbf_eventlab
