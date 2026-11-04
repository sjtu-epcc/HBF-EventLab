#include "hbf_eventlab/runtime/bank_cache.hpp"
namespace hbf_eventlab {
namespace {
bool same_page(PhysicalNandAddress a,PhysicalNandAddress b) {
    return block_of(a)==block_of(b) && a.page==b.page;
}
}
bool BankCache::contains(PhysicalNandAddress a) const {
    const auto it=banks_.find(bank_of(a)); if(it==banks_.end()) return false;
    for(const auto& s:it->second) if(!s.filling && s.tag && same_page(*s.tag,a)) return true;
    return false;
}
bool BankCache::can_fill(BankKey b) const {
    const auto it=banks_.find(b); if(it==banks_.end()) return true;
    for(const auto& s:it->second) if(!s.filling && !s.references) return true;
    return false;
}
std::optional<CacheFill> BankCache::begin_fill(PhysicalNandAddress a,const PageData* page) {
    auto& slots=banks_[bank_of(a)]; std::optional<std::uint32_t> victim;
    for(std::uint32_t i=0;i<2;++i) {
        const auto& s=slots[i]; if(s.filling || s.references) continue;
        if(!victim || (!s.tag && slots[*victim].tag) ||
           (s.tag.has_value()==slots[*victim].tag.has_value() && s.age<slots[*victim].age)) victim=i;
    }
    if(!victim) return {};
    auto& s=slots[*victim]; s.generation=Generation{checked_add(s.generation.value,1)};
    s.tag=a; s.tag->byte_offset=0; s.filling=true; ++filling_; s.fill_valid=page!=nullptr;
    if(page) s.data=*page;
    s.age=checked_add(age_,1); age_=s.age;
    return CacheFill{bank_of(a),*victim,s.generation};
}
BankCache::Slot& BankCache::get(CacheFill f) {
    auto& s=banks_.at(f.bank).at(f.slot);
    if(s.generation!=f.generation) throw std::logic_error("stale Cache slot generation");
    return s;
}
const BankCache::Slot& BankCache::get(CacheFill f) const {
    const auto& s=banks_.at(f.bank).at(f.slot);
    if(s.generation!=f.generation) throw std::logic_error("stale Cache slot generation");
    return s;
}
void BankCache::finish_fill(CacheFill f) {
    auto& s=get(f); if(!s.filling) throw std::logic_error("Cache fill completed twice");
    s.filling=false; --filling_; if(!s.fill_valid) s.tag.reset();
}
void BankCache::cancel_fill(CacheFill f) { auto& s=get(f); if(!s.filling) throw std::logic_error("fill already ended");
    s.filling=false; --filling_; s.tag.reset(); }
ReadResultLease BankCache::reference(CacheFill f,std::uint32_t offset) {
    if(offset>=dlu_bytes || offset%host_unit_bytes) throw std::invalid_argument("bad Cache fragment");
    auto& s=get(f); const ReadResultId id{checked_add(next_,1)};
    refs_.emplace(id,Ref{f,offset/host_unit_bytes}); if(s.references++==0) ++protected_; next_=id.value;
    s.age=checked_add(age_,1); age_=s.age;
    return ReadResultLease(*this,id);
}
ReadResultLease BankCache::pin_fill(CacheFill f,std::uint32_t offset) {
    if(!get(f).filling) throw std::logic_error("not a fill"); return reference(f,offset);
}
std::optional<ReadResultLease> BankCache::pin(PhysicalNandAddress a) {
    auto it=banks_.find(bank_of(a)); if(it==banks_.end()) return {};
    for(std::uint32_t i=0;i<2;++i) {
        const auto& s=it->second[i];
        if(!s.filling && s.tag && same_page(*s.tag,a))
            return reference({bank_of(a),i,s.generation},a.byte_offset);
    }
    return {};
}
void BankCache::store(ReadResultId id,DataToken token) {
    const auto& r=refs_.at(id); auto& s=get(r.slot);
    if(!s.filling) throw std::logic_error("cannot mutate a saved Cache result");
    s.data.at(r.fragment)=token;
}
DataToken BankCache::load(ReadResultId id) const {
    const auto& r=refs_.at(id); const auto& s=get(r.slot);
    if(s.filling) throw std::logic_error("Cache fill not finished");
    return s.data.at(r.fragment);
}
void BankCache::release(ReadResultId id) noexcept {
    const auto it=refs_.find(id); if(it==refs_.end()) return;
    if(--get(it->second.slot).references==0) --protected_; refs_.erase(it);
}
void BankCache::invalidate(PhysicalBlockKey b) noexcept {
    for(auto& bank:banks_) for(auto& s:bank.second)
        if(s.tag && block_of(*s.tag)==b) { s.tag.reset(); s.fill_valid=false; }
}
void BankCache::invalidate_page(PhysicalNandAddress a) noexcept {
    auto it=banks_.find(bank_of(a)); if(it==banks_.end()) return;
    for(auto& s:it->second) if(s.tag && same_page(*s.tag,a)) {s.tag.reset();s.fill_valid=false;}
}
void BankCache::invalidate_all() noexcept {
    for(auto& bank:banks_) for(auto& s:bank.second) { s.tag.reset(); s.fill_valid=false; }
}
std::size_t BankCache::used_bytes() const {
    std::size_t n=0; for(const auto& b:banks_) for(const auto& s:b.second)
        if(s.tag || s.filling || s.references) n+=dlu_bytes;
    return n;
}
} // namespace hbf_eventlab
