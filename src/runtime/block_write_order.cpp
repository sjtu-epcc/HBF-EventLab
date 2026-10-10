#include "hbf_eventlab/runtime/block_write_order.hpp"

namespace hbf_eventlab {
BlockWriteOrder::BlockWriteOrder(std::uint32_t pages) : pages_(pages) {
    if (!pages) throw std::invalid_argument("empty block");
}
bool BlockWriteOrder::can_start(PageId p) const {
    return mode_ == BlockMode::Normal && !collecting_ && p < pages_ && p == next_assembly_ &&
           (!replay_through() || p <= *replay_through());
}
void BlockWriteOrder::start_page(PageId p) {
    if (!can_start(p)) throw std::logic_error("Host page initiation violates current order interpretation");
    collecting_ = p;
}
void BlockWriteOrder::assembly_complete(PageId p) {
    if (collecting_ != std::optional<PageId>{p} || p != next_assembly_ || mode_ != BlockMode::Normal)
        throw std::logic_error("unexpected assembly completion");
    ready_.insert(p);
    collecting_.reset();
    ++next_assembly_;
}
void BlockWriteOrder::assembly_timeout(PageId p) {
    if (collecting_ != std::optional<PageId>{p}) throw std::logic_error("not the collecting page");
    collecting_.reset(); // Cursor did not advance: retry this page, not a fabricated full replay.
}
bool BlockWriteOrder::can_program(PageId p) const {
    return mode_ == BlockMode::Normal && !programming_ && p == next_program_ && ready_.count(p);
}
void BlockWriteOrder::start_program(PageId p) {
    if (!can_program(p)) throw std::logic_error("program order violation");
    ready_.erase(p);
    programming_ = p;
}
void BlockWriteOrder::program_success(PageId p) {
    if (programming_ != std::optional<PageId>{p}) throw std::logic_error("not the in-flight program");
    programming_.reset();
    ++next_program_;
    replay_.program_succeeded(p);
}
ProgramFailure BlockWriteOrder::program_failure(PageId p) {
    if (programming_ != std::optional<PageId>{p}) throw std::logic_error("not the in-flight program");
    ProgramFailure out{p, {ready_.begin(), ready_.end()}};
    if (collecting_) out.following_pages.push_back(*collecting_);
    replay_.program_failed(p);
    ready_.clear(); collecting_.reset(); programming_.reset();
    mode_ = BlockMode::AwaitingReplay;
    return out;
}
void BlockWriteOrder::restart_from_page0() {
    if (collecting_ || programming_ || !ready_.empty()) throw std::logic_error("block still has active order dependencies");
    // A restart during an unfinished replay does not discharge its prefix obligation.
    // Only successful program of the merged endpoint clears it.
    replay_.restart();
    next_assembly_ = next_program_ = 0;
    mode_ = BlockMode::Normal;
}
void BlockWriteOrder::discard_contents(Generation generation) {
    if (has_active_pages()) throw std::logic_error("discard with active pages");
    replay_.discard_contents(generation);
    next_assembly_ = next_program_ = 0;
    mode_ = BlockMode::Normal;
}
void BlockWriteOrder::retire() {
    if (has_active_pages()) throw std::logic_error("retire with active pages");
    replay_.retire();
    mode_ = BlockMode::Retired;
}
} // namespace hbf_eventlab
