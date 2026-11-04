#include "hbf_eventlab/host/recovery_manager.hpp"
namespace hbf_eventlab {
RecoveryTask& RecoveryManager::notice(ContentKey key,std::uint64_t pass,std::optional<PageId> root,const std::set<CommandId>& ids) {
    auto it=tasks_.find(key);
    if(it==tasks_.end() || it->second.phase==RecoveryPhase::Completed || it->second.phase==RecoveryPhase::Failed) {
        RecoveryTask task;task.id=RecoveryId{checked_add(next_,1)};next_=task.id.value;task.content=key;
        it=tasks_.insert_or_assign(key,std::move(task)).first;
    }
    auto& t=it->second;
    if(root)t.through=t.through?std::max(*t.through,*root):root;
    if(!t.failed_pass || *t.failed_pass!=pass) {
        t.failed_pass=pass;t.phase=RecoveryPhase::Draining;t.drain=ids; // fixed finite snapshot, every pass
    }
    return t;
}
void RecoveryManager::settled(LogicalBlockKey b,CommandId id) {
    for(auto& item:tasks_)if(item.first.block==b)item.second.drain.erase(id);
}
RecoveryTask* RecoveryManager::active(ContentKey k) {
    auto it=tasks_.find(k);
    if(it==tasks_.end() || it->second.phase==RecoveryPhase::Completed || it->second.phase==RecoveryPhase::Failed)return nullptr;
    return &it->second;
}
bool RecoveryManager::begin_pass(ContentKey k,std::uint64_t pass) {
    auto* t=active(k);
    if(!t || t->phase!=RecoveryPhase::Draining || !t->drain.empty() || !t->through)throw std::logic_error("replay before root/drain");
    if(t->passes>=limit_) {fail(k,"RecoveryBudgetExhausted");return false;}
    t->phase=RecoveryPhase::Replaying;t->pass=pass;t->page=0;t->pass_accepted=false;return true;
}
bool RecoveryManager::accepted(ContentKey k,std::uint64_t pass,PageId page) {
    auto* t=active(k);
    if(!t || t->phase!=RecoveryPhase::Replaying || t->pass!=pass || page || t->pass_accepted)return false;
    t->pass_accepted=true;++t->passes;return true;
}
void RecoveryManager::complete(ContentKey k) {auto& t=tasks_.at(k);t.phase=RecoveryPhase::Completed;t.drain.clear();}
void RecoveryManager::fail(ContentKey k,const std::string& why) {auto& t=tasks_.at(k);t.phase=RecoveryPhase::Failed;t.failure=why;t.drain.clear();}
std::size_t RecoveryManager::active_count() const {
    std::size_t n=0;for(const auto&i:tasks_)n+=i.second.phase==RecoveryPhase::Draining||i.second.phase==RecoveryPhase::Replaying;return n;
}
}

