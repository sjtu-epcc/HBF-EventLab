#pragma once
#include "hbf_eventlab/host/workload.hpp"
namespace hbf_eventlab {
class GlobalRequestSplitter {
public:
    GlobalRequestSplitter(const IHostInterleaver& i,const IAddressLayout& l):interleaver_(i),layout_(l) {}
    Fragment fragment(GlobalByteAddress,std::uint64_t content,LogicalWorkId={},GlobalRequestId={}) const;
    std::vector<Fragment> split(const GlobalRequest&) const;
private:
    const IHostInterleaver& interleaver_;
    const IAddressLayout& layout_;
};
} // namespace hbf_eventlab

