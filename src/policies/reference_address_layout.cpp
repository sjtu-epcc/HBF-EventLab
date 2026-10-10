#include "hbf_eventlab/policies/reference_address_layout.hpp"

namespace hbf_eventlab {
ReferenceAddressLayout::ReferenceAddressLayout(Geometry g) : geometry_(g) { g.validate(); }
NandSlotAddress ReferenceAddressLayout::decode(MediaAddress a) const {
    if (a.channel >= geometry_.host_channels || a.dlu.value >= geometry_.channel_dlus() || a.byte_offset >= dlu_bytes)
        throw std::out_of_range("media address outside geometry");
    auto x = a.dlu.value;
    const auto bank = static_cast<std::uint32_t>(x % geometry_.banks_per_die);
    x /= geometry_.banks_per_die;
    const auto die = static_cast<std::uint32_t>(x % geometry_.dies_per_channel);
    x /= geometry_.dies_per_channel;
    const auto page = static_cast<PageId>(x % geometry_.pages_per_block);
    return {a.channel, die, bank, x / geometry_.pages_per_block, page, a.byte_offset};
}
LogicalNandAddress ReferenceAddressLayout::logical_view(ChannelAddress a) const {
    const auto p = decode({a.channel, MediaDlu{a.local.value / dlu_bytes},
                           static_cast<std::uint32_t>(a.local.value % dlu_bytes)});
    return {{p.channel, p.die, p.bank, p.block}, p.page, p.byte_offset};
}
LocalByteAddress ReferenceAddressLayout::encode_logical(const LogicalNandAddress& a) const {
    const auto& b = a.block;
    if (b.channel >= geometry_.host_channels || b.die >= geometry_.dies_per_channel ||
        b.bank >= geometry_.banks_per_die || b.block >= geometry_.blocks_per_bank ||
        a.page >= geometry_.pages_per_block || a.byte_offset >= dlu_bytes)
        throw std::out_of_range("logical geometry outside profile");
    auto x = b.block * geometry_.pages_per_block + a.page;
    x = x * geometry_.dies_per_channel + b.die;
    x = x * geometry_.banks_per_die + b.bank;
    return LocalByteAddress{x * dlu_bytes + a.byte_offset};
}
} // namespace hbf_eventlab
