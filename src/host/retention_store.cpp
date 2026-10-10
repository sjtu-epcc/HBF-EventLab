#include "hbf_eventlab/host/retention_store.hpp"
#include <algorithm>
namespace hbf_eventlab {
void WriteRetentionStore::save(ContentKey key,const Fragment& f) {
    const RetainedKey k{key,f.logical.page,f.logical.byte_offset/host_unit_bytes};
    auto it=data_.find(k);
    if(it!=data_.end()) {
        if(it->second.token!=f.data || it->second.global!=f.global)throw std::logic_error("immutable content overwritten in retention");
        return;
    }
    if(!limit_)return;
    if(data_.size()==limit_) {
        auto victim=std::find_if(fifo_.begin(),fifo_.end(),[&](const auto& old){return !pinned_.count(old);});
        if(victim==fifo_.end())return; // active recovery is protected; new data may be unavailable for later recovery
        data_.erase(*victim);fifo_.erase(victim);
    }
    data_.emplace(k,RetainedFragment{f.global,f.data});fifo_.push_back(k);
}
bool WriteRetentionStore::pin_prefix(ContentKey key,PageId through) {
    for(PageId page=0;page<=through;++page)for(std::uint32_t slot=0;slot<64;++slot)
        if(!find(key,page,slot))return false;
    for(PageId page=0;page<=through;++page)for(std::uint32_t slot=0;slot<64;++slot)pinned_.insert({key,page,slot});
    return true;
}
void WriteRetentionStore::unpin(ContentKey key) {
    for(auto it=pinned_.begin();it!=pinned_.end();) {
        if(!(it->content<key)&&!(key<it->content))it=pinned_.erase(it);else ++it;
    }
}
std::optional<RetainedFragment> WriteRetentionStore::find(ContentKey key,PageId p,std::uint32_t slot) const {
    const auto it=data_.find({key,p,slot});return it==data_.end()?std::nullopt:std::optional<RetainedFragment>{it->second};
}
}

