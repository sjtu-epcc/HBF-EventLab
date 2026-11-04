#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <algorithm>
#include <optional>

namespace hbf_eventlab {
enum class ReplayPhase { Idle, AwaitingRestart, Replaying, Retired };
// One unfinished recovery obligation per logical block. Program/assembly ordering
// remains in BlockWriteOrder; responses and data remain owned by WritePath/Host.
class ReplayState {
public:
    ReplayPhase phase() const { return phase_; }
    std::optional<PageId> required_through() const { return required_through_; }
    std::optional<PageId> last_failure() const { return last_failure_; }
    std::uint64_t failure_count() const { return failure_count_; }
    std::optional<PhysicalBlockKey> target() const { return target_; }
    Generation content_generation() const { return content_generation_; }
    void program_failed(PageId p) {
        if (phase_ == ReplayPhase::Retired) throw std::logic_error("retired recovery");
        const auto count = checked_add(failure_count_, 1);
        required_through_ = required_through_ ? std::max(*required_through_, p) : p;
        last_failure_ = p; failure_count_ = count;
        phase_ = ReplayPhase::AwaitingRestart;
    }
    void restart() {
        if (phase_ == ReplayPhase::Retired) throw std::logic_error("retired recovery");
        phase_ = required_through_ ? ReplayPhase::Replaying : ReplayPhase::Idle;
    }
    // Replacement changes the target, but does not discharge the logical debt.
    void bind(PhysicalBlockKey target, Generation content_generation) {
        if (phase_ == ReplayPhase::Retired) throw std::logic_error("retired recovery");
        target_ = target; content_generation_ = content_generation;
    }
    // Called only after the current sequential program operation succeeds.
    void program_succeeded(PageId p) {
        if (phase_ == ReplayPhase::Replaying && required_through_ == std::optional<PageId>{p}) {
            required_through_.reset(); last_failure_.reset(); failure_count_ = 0;
            phase_ = ReplayPhase::Idle;
        }
    }
    // Only an explicit Host discard + committed remap can discard recovery debt.
    void discard_contents(Generation generation) {
        if (phase_ == ReplayPhase::Retired) throw std::logic_error("retirement is permanent");
        *this = ReplayState{}; content_generation_ = generation;
    }
    void retire() {
        required_through_.reset(); target_.reset(); phase_ = ReplayPhase::Retired;
    }
private:
    ReplayPhase phase_ = ReplayPhase::Idle;
    std::optional<PageId> required_through_, last_failure_;
    std::uint64_t failure_count_ = 0;
    std::optional<PhysicalBlockKey> target_;
    Generation content_generation_;
};
} // namespace hbf_eventlab
