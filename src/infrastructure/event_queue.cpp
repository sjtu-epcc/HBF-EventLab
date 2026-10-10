#include "hbf_eventlab/infrastructure/event_queue.hpp"
#include <algorithm>

namespace hbf_eventlab {
EventId EventQueue::schedule_at(Tick at, EventPhase phase, std::function<void()> callback) {
    if (at < now_ || !callback) throw std::invalid_argument("event must have callback and non-past time");
    if (phase != EventPhase::Completion && phase != EventPhase::Arrival && phase != EventPhase::Deadline)
        throw std::invalid_argument("unknown event phase");
    std::uint64_t round = 0;
    if (has_executed_ && at == now_) {
        round = round_;
        if (settling_ || phase <= active_phase_) round = checked_add(round,1);
    }
    const EventId id{checked_add(next_id_,1)};
    live_.insert(id);
    try { (phase == EventPhase::Deadline ? deadlines_ : normal_).push({at,round,phase,id,std::move(callback)}); }
    catch (...) { live_.erase(id); throw; }
    next_id_ = id.value;
    return id;
}
bool EventQueue::cancel(EventId id) { return live_.erase(id) != 0; }
void EventQueue::prune(Heap& q) {
    while (!q.empty() && !live_.count(q.top().id)) q.pop();
}
std::optional<Tick> EventQueue::next_time() {
    if (!settlement_.empty()) return now_;
    prune(normal_); prune(deadlines_);
    if (normal_.empty() && deadlines_.empty()) return std::nullopt;
    if (normal_.empty()) return deadlines_.top().at;
    if (deadlines_.empty()) return normal_.top().at;
    return std::min(normal_.top().at,deadlines_.top().at);
}
void EventQueue::check_budget(std::uint64_t count, std::uint64_t round) const {
    if ((limits_.total_events && checked_add(executed_,count) > limits_.total_events) ||
        (limits_.events_per_tick && checked_add(tick_events_,count) > limits_.events_per_tick) ||
        (limits_.rounds_per_tick && round >= limits_.rounds_per_tick))
        throw EventLimitReached("event limit reached; work preserved, no forced timeout");
}
void EventQueue::invoke(Event current) {
    live_.erase(current.id);
    round_ = std::max(round_,current.round);
    active_phase_ = current.phase;
    has_executed_ = true;
    ++executed_; ++tick_events_;
    executing_ = true;
    try { current.callback(); }
    catch (...) { executing_ = false; throw; }
    executing_ = false;
}
bool EventQueue::run_next() {
    if (executing_) throw std::logic_error("event runner is not reentrant");
    const auto next = next_time();
    if (!next) return false;
    if (limits_.total_events && executed_ >= limits_.total_events)
        throw EventLimitReached("total event limit reached");
    if (now_ < *next) { now_ = *next; round_ = tick_events_ = 0; has_executed_ = false; }
    if (settlement_.empty()) {
        if (!normal_.empty() && normal_.top().at == now_) {
            check_budget(1,normal_.top().round);
            auto e = normal_.top(); normal_.pop(); invoke(std::move(e)); return true;
        }
        // Freeze identities only after ALL normal rounds at this tick have drained.
        while (!deadlines_.empty() && deadlines_.top().at == now_) {
            settlement_.push_back(deadlines_.top()); deadlines_.pop(); prune(deadlines_);
        }
    }
    std::uint64_t count = 0;
    for (const auto& e : settlement_) if (live_.count(e.id)) ++count;
    check_budget(count,round_);
    settling_ = true;
    try {
        while (!settlement_.empty()) {
            auto e = std::move(settlement_.front()); settlement_.pop_front();
            if (live_.count(e.id)) { e.round = round_; invoke(std::move(e)); }
        }
    } catch (...) { settling_ = !settlement_.empty(); throw; }
    settling_ = false;
    return true;
}
void EventQueue::run() { while (run_next()) {} }
} // namespace hbf_eventlab

