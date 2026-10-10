#include "hbf_eventlab/policies/read_dispatch.hpp"
#include <utility>
#include <algorithm>

namespace hbf_eventlab {
Tick WindowDispatchPolicy::deadline(ReadWindowView v) const { return Tick{checked_add(v.first_arrival.value, wait_.value)}; }
bool WindowDispatchPolicy::ready(ReadWindowView v, Tick now) const {
    return v.size && ((threshold_ && v.size >= threshold_) || !(now < deadline(v)));
}
DispatchBatch ReadDispatchWindow::take() {
    window_=Generation{checked_add(window_.value,1)};
    return std::exchange(pending_, {});
}
std::vector<DispatchBatch> ReadDispatchWindow::arrive(CommandId id, ReadHint hint, Tick now) {
    return accept(id,hint,now);
}
std::vector<DispatchBatch> ReadDispatchWindow::accept(std::optional<CommandId> member, ReadHint hint, Tick now) {
    if (now < last_) throw std::logic_error("read arrivals moved backwards");
    if (hint!=ReadHint::Regular && hint!=ReadHint::Batch) throw std::invalid_argument("unknown read hint");
    if (member && std::find(pending_.begin(),pending_.end(),*member)!=pending_.end())
        throw std::logic_error("duplicate read window member");
    last_ = now;
    std::vector<DispatchBatch> out;
    if (!pending_.empty() && policy_.deadline(view()) < now) out.push_back(take());
    if (member) {
        if (pending_.empty()) first_ = now;
        pending_.push_back(*member);
    }
    // CHOSEN v0.7 interpretation (wording/bit-position ambiguity): B closes now.
    // R R B B R -> [R R B] [B], trailing R waits. This cannot override bank/resource legality.
    if (!pending_.empty() && (hint == ReadHint::Batch || policy_.ready(view(), now))) out.push_back(take());
    return out;
}
bool ReadDispatchWindow::remove(CommandId id) {
    const auto it=std::find(pending_.begin(),pending_.end(),id);
    if (it==pending_.end()) return false;
    pending_.erase(it);
    window_=Generation{checked_add(window_.value,1)};
    return true;
}
std::optional<ReadWindowWakeup> ReadDispatchWindow::wakeup() const {
    const auto at=next_wakeup();
    return at?std::optional<ReadWindowWakeup>{{window_,*at}}:std::nullopt;
}
std::optional<DispatchBatch> ReadDispatchWindow::flush_due(ReadWindowWakeup token,Tick now) {
    if (token.window!=window_ || !next_wakeup() || token.at!=*next_wakeup()) return {};
    return flush_due(now);
}
std::optional<DispatchBatch> ReadDispatchWindow::flush_due(Tick now) {
    if (now < last_) throw std::logic_error("read dispatch time moved backwards");
    last_ = now;
    if (!pending_.empty() && policy_.ready(view(), now)) return take();
    return std::nullopt;
}
std::optional<Tick> ReadDispatchWindow::next_wakeup() const {
    return pending_.empty() ? std::nullopt : std::optional<Tick>{policy_.deadline(view())};
}
} // namespace hbf_eventlab
