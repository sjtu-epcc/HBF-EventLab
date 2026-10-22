#include "hbf_eventlab/statistics/collector.hpp"
#include <ostream>
#include <set>
#include <stdexcept>

namespace hbf_eventlab {
namespace {
void require(bool ok,const char* why) { if(!ok)throw std::logic_error(why); }
void time_json(std::ostream& out,std::optional<Tick> t) { if(t)out<<t->value;else out<<"null"; }
void accepted_read(const AttemptTiming& a,Tick at) {
    require(a.command.op==Op::Read && a.accepted && !a.delivered && !(at<*a.accepted),
            "statistics: read observation outside accepted lifetime");
}
}
void StatisticsCollector::record(const StatReadWindowEntered& e) {
    accepted_read(attempts_.at(e.id),e.at);auto& r=read_paths_.at(e.id);
    require(!r.window_entered_at && !r.service,"statistics: duplicate/serviced window entry");
    r.window_entered_at=e.at;note(e.at);
}
void StatisticsCollector::record(const StatReadWindowRemoved& e) {
    accepted_read(attempts_.at(e.id),e.at);auto& r=read_paths_.at(e.id);
    require(r.window_entered_at && !r.window_left_at && !(e.at<*r.window_entered_at),
            "statistics: removing a nonmember read");
    r.window_left_at=e.at;note(e.at);
}
void StatisticsCollector::record(const StatReadWindowReleased& e) {
    (void)to_string(e.reason);
    require(!e.members.empty() && !(e.at<e.first_arrival) && !(e.deadline<e.first_arrival),
            "statistics: invalid read window release");
    std::set<TransferAttemptId> unique;
    // Validate the whole batch before mutating history.
    for(auto id:e.members) {
        const auto& a=attempts_.at(id);accepted_read(a,e.at);const auto& r=read_paths_.at(id);
        require(unique.insert(id).second && a.command.address.channel==e.channel &&
                r.window_entered_at && !r.window_left_at && !(e.at<*r.window_entered_at) &&
                !(*r.window_entered_at<e.first_arrival),"statistics: invalid read release member");
    }
    for(auto id:e.members) {auto& r=read_paths_.at(id);r.window_left_at=e.at;r.release_reason=e.reason;}
    read_window_releases_.push_back(e);note(e.at);
}
void StatisticsCollector::record(const StatReadServed& e) {
    const auto& a=attempts_.at(e.id);accepted_read(a,e.at);auto& r=read_paths_.at(e.id);
    require(!r.service && (!r.window_entered_at || (r.window_left_at && !(e.at<*r.window_left_at))),
            "statistics: duplicate service or read still in window");
    require(e.kind==ReadServiceKind::Sense || e.kind==ReadServiceKind::Cache,"statistics: unknown read service");
    if(e.kind==ReadServiceKind::Sense) {
        require(e.bank_ticket && r.release_reason,"statistics: sense without ticket/window release");
        const auto& bank=bank_operations_.at(*e.bank_ticket);
        require(bank.kind==MediaOperationKind::Read && bank.issued_at==e.at &&
                bank.target.channel==a.command.address.channel,"statistics: mismatched sense ticket");
    } else require(!e.bank_ticket,"statistics: cache service owns no Bank ticket");
    r.service=e.kind;r.service_at=e.at;r.bank_ticket=e.bank_ticket;note(e.at);
}
void StatisticsCollector::write_read_json(std::ostream& out) const {
    out<<",\"read_paths\":[";bool comma=false;
    for(const auto& entry:read_paths_) {
        if(comma)out<<',';comma=true;const auto& r=entry.second;
        out<<"{\"attempt\":"<<entry.first.value<<",\"window_entered_at\":";time_json(out,r.window_entered_at);
        out<<",\"window_left_at\":";time_json(out,r.window_left_at);
        out<<",\"release_reason\":";if(r.release_reason)out<<'"'<<to_string(*r.release_reason)<<'"';else out<<"null";
        out<<",\"service\":";if(r.service)out<<'"'<<(*r.service==ReadServiceKind::Cache?"cache":"sense")<<'"';else out<<"null";
        out<<",\"service_at\":";time_json(out,r.service_at);
        out<<",\"bank_ticket\":";if(r.bank_ticket)out<<*r.bank_ticket;else out<<"null";out<<'}';
    }
    out<<"],\"read_window_releases\":[";comma=false;
    for(const auto& e:read_window_releases_) {
        if(comma)out<<',';comma=true;
        out<<"{\"channel\":"<<e.channel<<",\"reason\":\""<<to_string(e.reason)<<"\",\"first_arrival\":"<<e.first_arrival.value
           <<",\"deadline\":"<<e.deadline.value<<",\"at\":"<<e.at.value<<",\"members\":[";
        bool sep=false;for(auto id:e.members){if(sep)out<<',';sep=true;out<<id.value;}out<<"]}";
    }
    out<<']';
}
} // namespace hbf_eventlab
