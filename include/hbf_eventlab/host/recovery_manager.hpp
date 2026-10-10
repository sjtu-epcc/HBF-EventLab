#pragma once
#include "hbf_eventlab/host/workload.hpp"
#include <set>
namespace hbf_eventlab {
using RecoveryId=Quantity<struct RecoveryTag>;
enum class RecoveryPhase { Draining, Replaying, Completed, Failed };
struct RecoveryTask {
    RecoveryId id;
    ContentKey content;
    RecoveryPhase phase=RecoveryPhase::Draining;
    std::optional<PageId> through; // no guessed root from a follower's 0x6/info2
    std::optional<std::uint64_t> failed_pass;
    std::set<CommandId> drain;
    std::uint32_t passes=0;
    std::uint64_t pass=0;
    PageId page=0;
    bool pass_accepted=false;
    std::string failure;
};
class RecoveryManager {
public:
    explicit RecoveryManager(std::uint32_t max_passes):limit_(max_passes) {}
    RecoveryTask& notice(ContentKey,std::uint64_t failed_pass,std::optional<PageId> root,
                         const std::set<CommandId>& unsettled);
    void settled(LogicalBlockKey,CommandId);
    RecoveryTask* active(ContentKey);
    const std::map<ContentKey,RecoveryTask>& tasks() const {return tasks_;}
    bool begin_pass(ContentKey,std::uint64_t pass);
    bool accepted(ContentKey,std::uint64_t pass,PageId);
    void complete(ContentKey);
    void fail(ContentKey,const std::string&);
    std::size_t active_count() const;
    std::uint64_t created() const {return next_;}
private:
    std::uint32_t limit_;
    std::uint64_t next_=0;
    std::map<ContentKey,RecoveryTask> tasks_;
};
} // namespace hbf_eventlab

