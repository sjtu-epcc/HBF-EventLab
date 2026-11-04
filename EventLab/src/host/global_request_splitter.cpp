#include "hbf_eventlab/host/global_request_splitter.hpp"
namespace hbf_eventlab {
Fragment GlobalRequestSplitter::fragment(GlobalByteAddress g,std::uint64_t content,LogicalWorkId id,GlobalRequestId parent) const {
    if(g.value%host_unit_bytes)throw std::invalid_argument("Global address must be 64B aligned");
    const auto local=interleaver_.map(g);
    if(local.local.value%host_unit_bytes)throw std::invalid_argument("interleaver must preserve aligned 64B commands");
    return {id,parent,g,local,layout_.logical_view(local),token_for(content,g)};
}
std::vector<Fragment> GlobalRequestSplitter::split(const GlobalRequest& r) const {
    RuntimeScope timing(profile_,RuntimeRegion::Split);
    if(!r.bytes || r.bytes%host_unit_bytes || r.address.value%host_unit_bytes)
        throw std::invalid_argument("Global range must be nonempty and 64B aligned");
    (void)checked_add(r.address.value,r.bytes);
    std::vector<Fragment> out;
    for(std::uint64_t offset=0;offset<r.bytes;offset+=host_unit_bytes) {
        out.push_back(fragment(GlobalByteAddress{checked_add(r.address.value,offset)},r.content,{},r.id));
        if(profile_)profile_->add(RuntimeCounter::SplitFragments);
    }
    return out;
}
}

