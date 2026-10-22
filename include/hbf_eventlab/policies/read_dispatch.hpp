#pragma once
#include "hbf_eventlab/interfaces/read_dispatch.hpp"
#include <optional>
#include <vector>
#include <functional>

namespace hbf_eventlab {
// threshold=0: fixed timer; threshold>0: queue depth OR timer (prevents starvation).
class WindowDispatchPolicy final : public IReadDispatchPolicy {
public:
    explicit WindowDispatchPolicy(Tick wait, std::size_t threshold = 0) : wait_(wait), threshold_(threshold) {}
    bool ready(ReadWindowView, Tick now) const override;
    Tick deadline(ReadWindowView) const override;
    ReadReleaseReason release_reason(ReadWindowView, Tick) const override;
private:
    Tick wait_;
    std::size_t threshold_;
};
using DispatchBatch = std::vector<CommandId>;
struct ReadWindowWakeup { Generation window; Tick at; };
// Owns accumulation only: cache hits bypass this component; no bank scheduling here.
class ReadDispatchWindow {
public:
    explicit ReadDispatchWindow(const IReadDispatchPolicy& policy) : policy_(policy) {}
    std::vector<DispatchBatch> arrive(CommandId, ReadHint, Tick now);
    // Accepted Batch can close a window even when this read was forwarded/errored.
    std::vector<DispatchBatch> accept(std::optional<CommandId> member, ReadHint, Tick now);
    bool remove(CommandId);
    std::optional<DispatchBatch> flush_due(Tick now);
    std::optional<DispatchBatch> flush_due(ReadWindowWakeup, Tick now);
    std::optional<Tick> next_wakeup() const;
    std::optional<ReadWindowWakeup> wakeup() const;
    std::size_t size() const { return pending_.size(); }
    using ReleaseObserver=std::function<void(const DispatchBatch&,ReadReleaseReason,Tick,Tick,Tick)>;
    // Observe actual release: members, reason, first arrival, deadline, now.
    void set_release_observer(ReleaseObserver observer) { observer_=std::move(observer); }
private:
    DispatchBatch take(ReadReleaseReason,Tick);
    ReadWindowView view() const { return {pending_.size(), first_}; }
    const IReadDispatchPolicy& policy_;
    DispatchBatch pending_;
    Tick first_, last_;
    Generation window_;
    ReleaseObserver observer_;
};
} // namespace hbf_eventlab
