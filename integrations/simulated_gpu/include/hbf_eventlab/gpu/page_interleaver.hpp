#pragma once
#include "hbf_eventlab/interfaces/address.hpp"
#include "page_placement.hpp"

namespace hbf_eventlab {
enum class GpuPagePlacement { Interleave, Contiguous, Grouped };
class GpuPageInterleaver final:public IHostInterleaver {
public:
    GpuPageInterleaver(unsigned channels,std::uint64_t channel_bytes,GpuPagePlacement mode=GpuPagePlacement::Interleave,
                      std::uint64_t group_pages=2,GlobalByteAddress base={})
        :channels_(channels),pages_(channel_bytes/4096),mode_(mode),group_(group_pages),base_(base) {
        if(!channels||!channel_bytes||channel_bytes%4096||!group_pages||
           (mode==GpuPagePlacement::Grouped&&pages_%group_pages))throw std::invalid_argument("invalid GPU page placement geometry");
        (void)checked_mul(channel_bytes,channels);(void)checked_mul(group_pages,channels);
    }
    ChannelAddress map(GlobalByteAddress address) const override {
        if(address<base_)throw std::invalid_argument("GPU address below base");
        const auto relative=address.value-base_.value,page=relative/4096,offset=relative%4096;
        if(page>=checked_mul(pages_,channels_))throw std::invalid_argument("GPU address outside HBF capacity");
        unsigned channel;std::uint64_t local;
        if(mode_==GpuPagePlacement::Contiguous){channel=hbf_sim_import::hbf_page_to_channel(channels_,1,page,pages_);local=page%pages_;}
        else if(mode_==GpuPagePlacement::Grouped){channel=static_cast<unsigned>((page/group_)%channels_);local=(page/(group_*channels_))*group_+page%group_;}
        else{channel=hbf_sim_import::hbf_page_to_channel(channels_,0,page,pages_);local=page/channels_;}
        return {channel,LocalByteAddress{checked_add(checked_mul(local,4096),offset)}};
    }
private:
    unsigned channels_;std::uint64_t pages_;GpuPagePlacement mode_;std::uint64_t group_;GlobalByteAddress base_;
};
} // namespace hbf_eventlab

