#include "hbf_eventlab/statistics/collector.hpp"
#include <ostream>
#include <stdexcept>

namespace hbf_eventlab {
void StatisticsCollector::record(const StatBankOperationStarted& e) {
    const auto key=std::make_tuple(e.target.channel,e.target.die,e.target.bank);
    if(!e.ticket||bank_operations_.count(e.ticket)||active_banks_.count(key))
        throw std::logic_error("statistics: duplicate ticket or overlapping Bank operation");
    if(e.kind!=MediaOperationKind::Read&&e.kind!=MediaOperationKind::Program&&e.kind!=MediaOperationKind::Erase)
        throw std::logic_error("statistics: invalid media operation kind");
    bank_operations_.emplace(e.ticket,BankTiming{e.ticket,e.kind,e.target,e.at,{}});
    active_banks_.emplace(key,e.ticket);note(e.at);
}
void StatisticsCollector::record(const StatBankOperationFinished& e) {
    auto& op=bank_operations_.at(e.ticket);
    if(op.completed_at||e.at<op.issued_at)throw std::logic_error("statistics: invalid Bank completion");
    const auto key=std::make_tuple(op.target.channel,op.target.die,op.target.bank);
    if(active_banks_.at(key)!=e.ticket)throw std::logic_error("statistics: stale Bank completion");
    op.completed_at=e.at;active_banks_.erase(key);note(e.at);
}
void StatisticsCollector::write_bank_json(std::ostream& out,Tick at) const {
    out<<",\"bank_operations\":[";bool comma=false;
    for(const auto& entry:bank_operations_) {
        if(comma)out<<',';comma=true;const auto& op=entry.second;const auto& a=op.target;
        const char* kind=op.kind==MediaOperationKind::Read?"sense":op.kind==MediaOperationKind::Program?"program":"erase";
        out<<"{\"ticket\":"<<op.ticket<<",\"kind\":\""<<kind<<"\",\"channel\":"<<a.channel<<",\"die\":"<<a.die
           <<",\"bank\":"<<a.bank<<",\"block\":"<<a.block<<",\"page\":"<<a.page<<",\"issued_at\":"<<op.issued_at.value
           <<",\"completed_at\":";
        if(op.completed_at)out<<op.completed_at->value;else out<<"null";
        out<<",\"observed_busy_ticks\":"<<(op.completed_at?op.completed_at->value:at.value)-op.issued_at.value<<'}';
    }
    out<<']';
}
} // namespace hbf_eventlab
