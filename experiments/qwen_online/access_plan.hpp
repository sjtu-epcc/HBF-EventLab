#pragma once
#include "hbf_eventlab/adapters/tensor_access_bridge.hpp"
#include <optional>

namespace qwen_online {
struct SelectedRange {
    std::string name;
    std::uint64_t tensor_bytes,offset,bytes;
    void validate() const {
        if(name.empty()||!tensor_bytes||!bytes||offset%hbf_eventlab::host_unit_bytes||
           bytes%hbf_eventlab::dlu_bytes||offset>tensor_bytes||bytes>tensor_bytes-offset)
            throw std::invalid_argument("selection needs an in-tensor 64B-aligned offset and whole 4KiB pages");
    }
    // Returned offset is relative to the registered slice, not the whole tensor.
    std::optional<std::pair<std::uint64_t,std::uint64_t>> intersection(
        const std::string& tensor,std::uint64_t at,std::uint64_t count) const {
        if(tensor!=name)return {};
        if(!count||at>tensor_bytes||count>tensor_bytes-at)
            throw std::invalid_argument("executor callback outside tensor");
        const auto begin=std::max(offset,at),end=std::min(offset+bytes,at+count);
        if(begin>=end)return {};
        if(begin%hbf_eventlab::host_unit_bytes||(end-begin)%hbf_eventlab::host_unit_bytes)
            throw std::invalid_argument("selected callback intersection is not 64B aligned");
        return std::make_pair(begin-offset,end-begin);
    }
};
}
