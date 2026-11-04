#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <deque>
#include <functional>
#include <optional>
#include <queue>
#include <set>
#include <vector>

namespace hbf_eventlab {
// Deadline is exclusively assembly settlement. Window timers are normal events.
enum class EventPhase : std::uint8_t { Completion, Arrival, Deadline };
struct EventLimits {
    std::uint64_t total_events = 0; // 0: unlimited
    std::uint64_t events_per_tick = 100000;
    std::uint64_t rounds_per_tick = 100000;
};
class EventLimitReached : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
class EventQueue {
public:
    EventId schedule_at(Tick, EventPhase, std::function<void()>);
    bool cancel(EventId);
    // Settlement batches are indivisible with respect to feedback and limit checks.
    // Callback exceptions retain the remaining batch; resumption finishes it first.
    bool run_next();
    void run();
    // Advance an empty time interval without inventing an event. Events at the
    // endpoint remain unexecuted. Never skip a live event or frozen settlement.
    void advance_idle_to(Tick);
    void set_limits(EventLimits limits) { limits_ = limits; }
    Tick now() const { return now_; }
    std::uint64_t round() const { return round_; }
    std::uint64_t executed() const { return executed_; }
    std::uint64_t executed_this_tick() const { return tick_events_; }
    bool settling_deadlines() const { return settling_; }
    bool tick_started() const { return has_executed_ || !settlement_.empty(); }
    std::size_t pending() const { return live_.size(); }
    std::optional<Tick> next_time();
private:
    struct Event { Tick at; std::uint64_t round; EventPhase phase; EventId id; std::function<void()> callback; };
    struct Later {
        bool operator()(const Event& a, const Event& b) const {
            return std::tie(a.at,a.round,a.phase,a.id) > std::tie(b.at,b.round,b.phase,b.id);
        }
    };
    using Heap = std::priority_queue<Event,std::vector<Event>,Later>;
    void prune(Heap&);
    void check_budget(std::uint64_t count, std::uint64_t round) const;
    void invoke(Event);
    Tick now_;
    std::uint64_t next_id_ = 0, round_ = 0, executed_ = 0, tick_events_ = 0;
    bool executing_ = false, has_executed_ = false, settling_ = false;
    EventPhase active_phase_ = EventPhase::Completion;
    EventLimits limits_;
    Heap normal_, deadlines_;
    std::deque<Event> settlement_;
    std::set<EventId> live_;
};
} // namespace hbf_eventlab

