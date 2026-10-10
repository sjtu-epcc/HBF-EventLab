#pragma once
#include "hbf_eventlab/interfaces/execution_policy.hpp"
#include <map>
namespace hbf_eventlab {
struct BankKey {
    ChannelId channel; std::uint32_t die,bank;
    friend bool operator<(BankKey a,BankKey b) { return std::tie(a.channel,a.die,a.bank)<std::tie(b.channel,b.die,b.bank); }
};
inline BankKey bank_of(PhysicalNandAddress a) { return {a.channel,a.die,a.bank}; }
inline BankKey bank_of(PhysicalBlockKey a) { return {a.channel,a.die,a.bank}; }
struct BankOperation { std::uint64_t ticket; MediaOperationKind kind; Tick issued; };
class DeviceRuntimeState {
public:
    bool available(BankKey b) const { return busy_.find(b)==busy_.end(); }
    std::uint64_t acquire(BankKey b,MediaOperationKind k,Tick t) {
        if(!available(b)) throw std::logic_error("Bank double issue");
        const auto id=checked_add(next_,1); busy_.emplace(b,BankOperation{id,k,t}); next_=id; return id;
    }
    void release(BankKey b,std::uint64_t id) {
        const auto it=busy_.find(b);
        if(it==busy_.end() || it->second.ticket!=id) throw std::logic_error("stale Bank completion");
        busy_.erase(it);
    }
    std::size_t active() const { return busy_.size(); }
private:
    std::map<BankKey,BankOperation> busy_;
    std::uint64_t next_=0;
};
} // namespace hbf_eventlab

