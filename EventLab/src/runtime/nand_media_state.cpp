#include "hbf_eventlab/runtime/nand_media_state.hpp"

namespace hbf_eventlab {
void NandMediaState::validate(PhysicalBlockKey k) const {
    if (k.channel >= geometry_.host_channels || k.die >= geometry_.dies_per_channel ||
        k.bank >= geometry_.banks_per_die || k.block >= geometry_.physical_blocks_per_bank())
        throw std::out_of_range("physical block outside media including spares");
}
void NandMediaState::erase(PhysicalBlockKey k) {
    validate(k); auto& b = blocks_[k];
    if (b.unusable) throw std::logic_error("erase on unusable physical block");
    const auto next = checked_add(b.erases, 1);
    b.pages.clear(); b.next_page = 0; b.erases = next;
}
void NandMediaState::program(PhysicalNandAddress a, const PageData& data) {
    const auto k = block_of(a); validate(k);
    if (a.page >= geometry_.pages_per_block || a.byte_offset) throw std::invalid_argument("program needs page base");
    auto& b = blocks_[k];
    if (b.unusable || a.page != b.next_page) throw std::logic_error("illegal media program");
    if (!b.pages.emplace(a.page, data).second) throw std::logic_error("programmed physical page requires erase");
    ++b.next_page;
}
const PageData* NandMediaState::read(PhysicalNandAddress a) const {
    validate(block_of(a));
    if (a.page >= geometry_.pages_per_block || a.byte_offset >= dlu_bytes) throw std::out_of_range("media page/offset");
    auto it = blocks_.find(block_of(a));
    if (it == blocks_.end()) return nullptr;
    if (it->second.unusable) throw std::logic_error("read from unusable physical block requires Host backup");
    auto p = it->second.pages.find(a.page);
    return p == it->second.pages.end() ? nullptr : &p->second;
}
void NandMediaState::mark_unusable(PhysicalBlockKey k) { validate(k); blocks_[k].unusable = true; }
bool NandMediaState::unusable(PhysicalBlockKey k) const {
    validate(k); auto it = blocks_.find(k); return it != blocks_.end() && it->second.unusable;
}
std::uint64_t NandMediaState::erase_count(PhysicalBlockKey k) const {
    validate(k); auto it = blocks_.find(k); return it == blocks_.end() ? 0 : it->second.erases;
}
} // namespace hbf_eventlab
