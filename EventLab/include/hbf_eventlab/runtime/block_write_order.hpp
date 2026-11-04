#pragma once
#include "hbf_eventlab/runtime/replay_state.hpp"
#include <optional>
#include <set>
#include <vector>

namespace hbf_eventlab {
enum class BlockMode { Normal, AwaitingReplay, Retired };
struct ProgramFailure { PageId failed_page; std::vector<PageId> following_pages; };
// One instance per LOGICAL block. WritePath binds recovery to a pinned physical
// target and performs media legality checks; this class owns order and replay debt.
class BlockWriteOrder {
public:
    explicit BlockWriteOrder(std::uint32_t pages_per_block);
    bool can_start(PageId) const;
    void start_page(PageId);
    void assembly_complete(PageId);
    void assembly_timeout(PageId);
    bool can_program(PageId) const;
    void start_program(PageId);
    void program_success(PageId);
    ProgramFailure program_failure(PageId);
    // Coordinator checks drain/response/read dependencies first. No block epoch.
    // CHOSEN: program-fail replay covers page 0 through the failed page (§11.5.2.7).
    void restart_from_page0();
    void bind_target(PhysicalBlockKey target, Generation contents) { replay_.bind(target, contents); }
    void discard_contents(Generation);
    void retire();
    const ReplayState& replay() const { return replay_; }
    PageId next_page_to_assemble() const { return next_assembly_; }
    PageId next_page_to_program() const { return next_program_; }
    BlockMode mode() const { return mode_; }
    bool has_active_pages() const { return collecting_ || programming_ || !ready_.empty(); }
    std::optional<PageId> replay_through() const { return replay_.required_through(); }
private:
    std::uint32_t pages_;
    PageId next_assembly_ = 0, next_program_ = 0;
    std::optional<PageId> collecting_, programming_;
    std::set<PageId> ready_;
    BlockMode mode_ = BlockMode::Normal;
    ReplayState replay_;
};
} // namespace hbf_eventlab
