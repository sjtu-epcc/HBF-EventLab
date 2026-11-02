#pragma once
#include "hbflab/frontier.hpp"

namespace hbflab {
inline constexpr std::uint32_t max_async_producers=8192;
// A parked producer has no permission to timestamp a request. Only Granted
// producers contribute unknown arrivals. Pending timestamps are already known.
class AsyncOrder {
public:
    enum class State { Parked, Granted, Pending, InFlight };
    struct Slot {
        State state=State::Parked;
        std::uint64_t ticket=0, floor_ns=0, floor_ps=0, arrival_ns=0, arrival_ps=0;
    };
    AsyncOrder(std::uint32_t count,std::uint64_t gpu_epoch,std::uint64_t model_epoch,std::uint32_t scale)
        : gpu_(gpu_epoch),model_(model_epoch),scale_(scale) {
        if(!count || count>max_async_producers || !scale || scale>1000000)
            throw std::invalid_argument("invalid asynchronous timeline");
        slots_.resize(count);
    }
    std::uint64_t map(std::uint64_t ns) const {
        if(ns<gpu_)throw std::invalid_argument("timestamp before GPU epoch");
        const auto result=add(model_,scale_ticks(ns-gpu_,1000,scale_));
        if(result==UINT64_MAX)throw std::overflow_error("infinite watermark is reserved");
        return result;
    }
    std::uint64_t grant(std::uint32_t producer,std::uint64_t ticket,std::uint64_t now) {
        auto& s=slots_.at(producer);
        if(s.state!=State::Parked || s.ticket==UINT64_MAX || ticket!=s.ticket+1 || now<model_)
            throw std::invalid_argument("invalid admission owner/ticket/state");
        const auto floor=add(gpu_,scale_ticks(add(std::max(now,sealed_),1)-model_,scale_,1000,true));
        const auto ps=map(floor);
        s={State::Granted,ticket,floor,ps,0,0};return floor;
    }
    void publish(std::uint32_t producer,std::uint64_t ticket,std::uint64_t ns) {
        auto& s=slots_.at(producer);
        if(s.state!=State::Granted || ticket!=s.ticket || ns<s.floor_ns)
            throw std::invalid_argument("ungranted, duplicate or backdated request");
        const auto ps=map(ns);
        s.arrival_ns=ns;s.arrival_ps=ps;s.state=State::Pending;
    }
    // Unknown arrival floors seal only strictly earlier model ticks. This also
    // groups timestamps which collapse to the same tick under time scaling.
    std::uint64_t unknown_floor() const {
        auto bound=UINT64_MAX;
        for(const auto& s:slots_)if(s.state==State::Granted)bound=std::min(bound,s.floor_ps);
        return bound;
    }
    std::vector<FrontierItem> ready() {
        const auto bound=unknown_floor();std::vector<FrontierItem> result;
        for(std::uint32_t i=0;i<slots_.size();++i) {
            auto& s=slots_[i];
            if(s.state==State::Pending && s.arrival_ps<bound)
                result.push_back({i,s.ticket,s.arrival_ns,s.arrival_ps});
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){
            return std::tie(a.arrival_ns,a.producer,a.ticket)<std::tie(b.arrival_ns,b.producer,b.ticket);
        });
        for(const auto& r:result){slots_[r.producer].state=State::InFlight;sealed_=std::max(sealed_,r.arrival_ps);}
        return result;
    }
    void complete(std::uint32_t producer,std::uint64_t ticket) {
        auto& s=slots_.at(producer);
        if(s.state!=State::InFlight || s.ticket!=ticket)throw std::invalid_argument("invalid completion identity");
        s.state=State::Parked;
    }
    const Slot& slot(std::uint32_t producer) const {return slots_.at(producer);}
    bool idle() const {for(const auto& s:slots_)if(s.state!=State::Parked)return false;return true;}
private:
    static std::uint64_t add(std::uint64_t a,std::uint64_t b) {
        if(b>UINT64_MAX-a)throw std::overflow_error("asynchronous clock overflow");return a+b;
    }
    std::vector<Slot> slots_;
    std::uint64_t gpu_,model_,sealed_=0;
    std::uint32_t scale_;
};
}
