#include "hbflab/capacity.hpp"
#include "page_file.hpp"
#include "hbf_eventlab/policies/reference_address_layout.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <thread>
#include <cmath>

namespace hbflab {
namespace {
std::atomic<std::uint64_t> next_store{1};
constexpr auto absent=(std::numeric_limits<std::size_t>::max)();
using Page=std::array<unsigned char,capacity_page_bytes>;
void bounds(std::size_t offset,std::size_t bytes,std::size_t size,const void* p) {
    if(offset>size || bytes>size-offset || (bytes && !p))throw std::invalid_argument("capacity byte range");
}
CapacityOptions validate(CapacityOptions o) {
    if(!o.logical_pages || o.logical_pages>262144 || !o.frames || o.frames>o.logical_pages ||
       !o.max_pins || o.max_pins>65536 || o.logical_pages>o.backend.geometry.channel_bytes()/4096)
        throw std::invalid_argument("invalid capacity/frame budget");
    if(!o.prefetch_budget || o.prefetch_budget>64 || o.read_ahead_pages>o.prefetch_budget ||
       (o.backing_mode!=BackingMode::Memory && o.backing_mode!=BackingMode::CreateFile && o.backing_mode!=BackingMode::OpenFile) ||
       (o.backing_mode!=BackingMode::Memory && o.backing_path.empty()))
        throw std::invalid_argument("invalid backing/prefetch options");
    return o;
}
}
struct CapacityStore::Impl {
    struct Backing { std::uint64_t owner=0;bool zero=false,expired=false;double programmed=0; };
    struct Frame { Page data{};std::size_t page=absent,pins=0;bool dirty=false,writer=false,prefetched=false;std::uint64_t generation=0,revision=0,used=0; };
    struct Object { std::size_t bytes;std::vector<std::size_t> pages; };
    struct Held { std::uint64_t object;PageInfo info; };
    CapacityOptions options;
    std::unique_ptr<TimingBackend> backend;
    CapacityMedia* shared=nullptr;
    std::uint64_t logical_base=0;
    std::vector<Backing> backing;
    std::vector<Page> memory;
    std::unique_ptr<PageFile> file;
    EnvironmentModel environment;
    double gpu_watts=0;
    std::vector<Frame> frames;
    std::map<std::uint64_t,Object> objects;
    std::map<std::uint64_t,Held> leases;
    const std::thread::id owner=std::this_thread::get_id();
    const std::uint64_t identity=next_store.fetch_add(1);
    std::uint64_t serial=0,clock=0,token=0;
    CapacityStats stats;
    explicit Impl(CapacityOptions o,CapacityMedia* media):options(validate(o)),shared(media),backing(options.logical_pages),environment(options.environment),frames(options.frames) {
        if(shared && options.environment.enabled) {
            const auto geometry=shared->media_geometry();
            if(!geometry)throw std::invalid_argument("retention requires the shared owner's media geometry");
            options.backend.geometry=*geometry;
        }
        if(options.backing_mode==BackingMode::Memory)memory.resize(options.logical_pages);
        else {
            file=std::make_unique<PageFile>(options.backing_path,options.logical_pages,options.backing_mode==BackingMode::CreateFile);
            serial=file->serial();
            for(std::size_t p=0;p<backing.size();++p) {
                const auto e=file->entries()[p];
                if(!e.owner){if(e.bytes)throw std::runtime_error("invalid backing catalog free entry");continue;}
                if(e.owner>serial || !e.bytes || e.bytes>backing.size()*4096)throw std::runtime_error("invalid backing object");
                auto it=objects.emplace(e.owner,Object{static_cast<std::size_t>(e.bytes),{}}).first;
                if(it->second.bytes!=e.bytes)throw std::runtime_error("inconsistent backing object size");
                it->second.pages.push_back(p);backing[p].owner=e.owner;
            }
            for(const auto& item:objects)if(item.second.pages.size()!=(item.second.bytes+4095)/4096)throw std::runtime_error("incomplete backing object");
            if(options.backing_mode==BackingMode::OpenFile)for(auto& p:backing)if(!p.owner)p.zero=true;
        }
        if(!shared)backend=std::make_unique<TimingBackend>(options.backend);
        logical_base=shared?shared->reserve_media(options.logical_pages*4096):backend->reserve(options.logical_pages*4096);
        token=logical_base/64+options.logical_pages*64;
        observe();
    }
    BackendStats media_stats() const{return shared?shared->media_statistics():backend->statistics();}
    std::uint64_t now() const{return shared?shared->media_time_ps():backend->now_ps();}
    void poison(){stats.poisoned=true;if(shared)shared->abort_media();}
    void current(bool allow_poison=false) const {
        if(owner!=std::this_thread::get_id())throw std::logic_error("capacity host owner changed");
        if(stats.poisoned && !allow_poison)throw std::runtime_error("capacity session is poisoned");
    }
    std::uint64_t next(){return serial=hbf_eventlab::checked_add(serial,1);}
    void mutation(){if(file)try{file->dirty();}catch(...){poison();throw;}}
    Page bytes(std::size_t page) {
        Page data{};if(file){file->read(page,data);stats.file_read_bytes+=4096;}else data=memory[page];return data;
    }
    void observe(){if(options.environment.enabled){const auto sample=shared?shared->sample_media():MediaSample{backend->now_ps(),backend->statistics()};environment.sample(sample.time_ps,sample.stats,gpu_watts);}}
    void programmed(std::size_t page) {
        if(!options.environment.enabled)return;
        // EventLab write_page replays the entire initialized physical block.
        // Reset every rewritten page's age, including other allocations.
        const hbf_eventlab::ReferenceAddressLayout layout(options.backend.geometry);
        const auto block=layout.logical_view({0,hbf_eventlab::LocalByteAddress{logical_base+page*4096}}).block;
        for(std::uint32_t p=0;p<options.backend.geometry.pages_per_block;++p) {
            const auto at=layout.encode_logical({block,p,0}).value;
            if(at>=logical_base && at-logical_base<backing.size()*4096) {
                auto& entry=backing[(at-logical_base)/4096];entry.programmed=environment.statistics().exposure;entry.expired=false;
            }
        }
    }
    Object& object(Allocation a) {
        current();const auto it=objects.find(a.value);
        if(a.store!=identity || it==objects.end())throw std::invalid_argument("foreign/stale capacity allocation");return it->second;
    }
    Held& held(PageLease l) {
        current();const auto it=leases.find(l.value);
        if(l.store!=identity || it==leases.end())throw std::invalid_argument("foreign/stale page lease");
        const auto& f=frames.at(it->second.info.frame);
        if(f.generation!=it->second.info.generation || f.page!=it->second.info.page || !f.pins)
            throw std::logic_error("page lease generation mismatch");
        return it->second;
    }
    void writeback(Frame& f) {
        if(!f.dirty)return;
        std::array<std::uint64_t,64> tokens{};
        try {
            mutation();
            for(auto& value:tokens){token=hbf_eventlab::checked_add(token,1);value=token;}
            const auto address=logical_base+f.page*4096;
            const auto result=shared?shared->write_media(address,tokens):backend->write_page(address,tokens);
            if(result.status!=Status::Ok)throw std::runtime_error("capacity writeback failed");
            observe();
            if(file){file->write(f.page,f.data);stats.file_write_bytes+=4096;}else memory[f.page]=f.data;
            backing[f.page].zero=false;f.dirty=false;++stats.writebacks;programmed(f.page);
        } catch(...){poison();throw;}
    }
};
CapacityStore::CapacityStore(CapacityOptions o,CapacityMedia* media):impl_(std::make_unique<Impl>(o,media)){}
CapacityStore::~CapacityStore()=default;
Allocation CapacityStore::allocate(std::size_t bytes) {
    auto& s=*impl_;s.current();
    if(!bytes || bytes>s.backing.size()*4096)throw std::invalid_argument("invalid allocation size");
    const auto count=(bytes+4095)/4096;
    Impl::Object object{bytes,{}};object.pages.reserve(count);
    for(std::size_t i=0;i<s.backing.size() && object.pages.size()<count;++i)if(!s.backing[i].owner)object.pages.push_back(i);
    if(object.pages.size()!=count)throw CapacityBusy("logical capacity exhausted");
    const auto id=s.next();auto it=s.objects.emplace(id,std::move(object)).first;
    s.mutation();
    for(auto page:it->second.pages)s.backing[page].owner=id;
    return {s.identity,id};
}
std::size_t CapacityStore::size(Allocation a) const{return impl_->object(a).bytes;}
std::size_t CapacityStore::free_pages() const{auto& s=*impl_;s.current();return std::count_if(s.backing.begin(),s.backing.end(),[](const auto& p){return !p.owner;});}
std::size_t CapacityStore::frame_count() const{impl_->current();return impl_->frames.size();}
std::vector<Allocation> CapacityStore::allocations() const {auto& s=*impl_;s.current();std::vector<Allocation> out;for(const auto& o:s.objects)out.push_back({s.identity,o.first});return out;}
std::size_t CapacityStore::pin_count(Allocation a) const {
    auto& s=*impl_;s.object(a);std::size_t count=0;for(const auto& l:s.leases)if(l.second.object==a.value)++count;return count;
}
void CapacityStore::release(Allocation a) {
    auto& s=*impl_;auto& o=s.object(a);if(pin_count(a))throw CapacityBusy("cannot discard pinned allocation");
    s.mutation();
    for(auto page:o.pages) {
        for(auto& f:s.frames)if(f.page==page){if(f.dirty)++s.stats.discarded_dirty;if(f.prefetched)++s.stats.prefetch_wasted;f.page=absent;f.dirty=false;f.prefetched=false;}
        s.backing[page].owner=0;s.backing[page].zero=true; // Reuse is logically zero, never old owner bytes.
    }
    s.objects.erase(a.value);
}
PageLease CapacityStore::pin(Allocation a,std::size_t index,PageAccess access) {
    const auto lease=pin_impl(a,index,access,false);
    auto& s=*impl_;
    try {
        if(s.options.read_ahead_pages && access==PageAccess::Read) {
            const auto pages=s.object(a).pages.size();
            prefetch(a,index+1,(std::min)(s.options.read_ahead_pages,pages-index-1));
        }
    }catch(...){if(!s.stats.poisoned)unpin(lease);throw;}
    return lease;
}
PageLease CapacityStore::pin_impl(Allocation a,std::size_t index,PageAccess access,bool speculative) {
    auto& s=*impl_;auto& o=s.object(a);
    if(index>=o.pages.size() || (access!=PageAccess::Read && access!=PageAccess::Write))throw std::invalid_argument("invalid page pin");
    if(s.leases.size()>=s.options.max_pins)throw CapacityBusy("pin budget exhausted");
    const auto page=o.pages[index];std::size_t slot=absent;
    for(std::size_t i=0;i<s.frames.size();++i)if(s.frames[i].page==page){slot=i;break;}
    if(slot!=absent) {
        const auto& f=s.frames[slot];if(f.writer || (access==PageAccess::Write && f.pins))throw CapacityBusy("page writer must be exclusive");
        ++s.stats.hits;
        if(!speculative && s.frames[slot].prefetched){s.frames[slot].prefetched=false;++s.stats.prefetch_useful;}
    } else {
        for(std::size_t i=0;i<s.frames.size();++i) {
            const auto& f=s.frames[i];if(f.pins)continue;
            if(speculative && (f.dirty || (f.page!=absent && !f.prefetched && !s.options.prefetch_evict_clean)))continue;
            if(slot==absent || f.page==absent || (s.frames[slot].page!=absent && f.used<s.frames[slot].used))slot=i;
            if(f.page==absent)break;
        }
        if(slot==absent)throw CapacityBusy("all frames pinned; release before retry");
        auto& f=s.frames[slot];s.writeback(f);
        Page data{};
        try {
            if(!s.backing[page].zero) {
                const auto address=s.logical_base+page*4096;
                const auto result=s.shared?s.shared->read_media(address,4096):s.backend->read(address,4096);
                if(result.status!=Status::Ok)throw std::runtime_error("capacity page-in failed");
                s.observe();data=s.bytes(page);
            }
        } catch(...){s.poison();throw;}
        if(f.page!=absent)++s.stats.evictions;
        if(f.prefetched)++s.stats.prefetch_wasted;
        f.data=data;f.page=page;f.dirty=s.backing[page].zero;f.prefetched=speculative;
        f.generation=hbf_eventlab::checked_add(f.generation,1);f.revision=0;++s.stats.faults;
    }
    auto& f=s.frames[slot];const auto id=s.next();
    PageInfo info{slot,page,(std::min)(std::size_t{4096},o.bytes-index*4096),f.generation,f.revision,access};
    s.leases.emplace(id,Impl::Held{a.value,info});++f.pins;f.writer=access==PageAccess::Write;
    f.used=s.clock=hbf_eventlab::checked_add(s.clock,1);
    s.stats.peak_pins=(std::max)(s.stats.peak_pins,s.leases.size());return {s.identity,id};
}
PageInfo CapacityStore::info(PageLease l) const{auto info=impl_->held(l).info;info.revision=impl_->frames[info.frame].revision;return info;}
void CapacityStore::read_pinned(PageLease l,std::size_t offset,void* out,std::size_t bytes) const {
    auto& s=*impl_;const auto& h=s.held(l);bounds(offset,bytes,h.info.bytes,out);
    if(bytes)std::memcpy(out,s.frames[h.info.frame].data.data()+offset,bytes);
}
void CapacityStore::write_pinned(PageLease l,std::size_t offset,const void* in,std::size_t bytes) {
    auto& s=*impl_;const auto& h=s.held(l);bounds(offset,bytes,h.info.bytes,in);
    if(h.info.access!=PageAccess::Write)throw std::invalid_argument("write through read lease");
    auto& f=s.frames[h.info.frame];if(bytes){s.mutation();const auto revision=hbf_eventlab::checked_add(f.revision,1);std::memcpy(f.data.data()+offset,in,bytes);f.dirty=true;f.revision=revision;}
}
void CapacityStore::unpin(PageLease l) {
    auto& s=*impl_;const auto& h=s.held(l);auto& f=s.frames[h.info.frame];--f.pins;f.writer=false;s.leases.erase(l.value);
}
void CapacityStore::read(Allocation a,std::size_t offset,void* out,std::size_t bytes) {
    bounds(offset,bytes,size(a),out);auto* p=static_cast<unsigned char*>(out);
    while(bytes) {
        const auto n=(std::min)(bytes,4096-offset%4096);const auto l=pin(a,offset/4096);
        try {read_pinned(l,offset%4096,p,n);}catch(...){unpin(l);throw;}unpin(l);
        offset+=n;p+=n;bytes-=n;
    }
}
void CapacityStore::write(Allocation a,std::size_t offset,const void* in,std::size_t bytes) {
    bounds(offset,bytes,size(a),in);const auto* p=static_cast<const unsigned char*>(in);
    // Multi-page writes are ordered, not atomic; on media failure the session is terminal.
    while(bytes) {
        const auto n=(std::min)(bytes,4096-offset%4096);const auto l=pin(a,offset/4096,PageAccess::Write);
        try {write_pinned(l,offset%4096,p,n);}catch(...){unpin(l);throw;}unpin(l);
        offset+=n;p+=n;bytes-=n;
    }
}
void CapacityStore::flush(Allocation a) {
    auto& s=*impl_;auto& object=s.object(a);if(pin_count(a))throw CapacityBusy("flush requires released leases");
    // Reused, untouched pages are logically zero. Materialize them before reporting
    // flush completion, so old-owner media contents never survive a successful flush.
    for(std::size_t index=0;index<object.pages.size();++index)if(s.backing[object.pages[index]].zero) {
        const auto lease=pin(a,index);const auto frame=info(lease).frame;unpin(lease);s.writeback(s.frames[frame]);
    }
    for(auto& f:s.frames)if(f.page!=absent && s.backing[f.page].owner==a.value)s.writeback(f);
}
void CapacityStore::flush_all() {
    auto& s=*impl_;s.current();if(!s.leases.empty())throw CapacityBusy("flush requires released leases");
    for(const auto& object:s.objects)flush({s.identity,object.first});
    if(s.file)try {
        std::vector<PageFile::Entry> catalog(s.backing.size());
        for(const auto& item:s.objects)for(auto p:item.second.pages)catalog[p]={item.first,item.second.bytes};
        s.file->checkpoint(catalog,s.serial);++s.stats.checkpoints;
    }catch(...){s.poison();throw;}
}
std::size_t CapacityStore::prefetch(Allocation a,std::size_t first,std::size_t count) {
    auto& s=*impl_;auto& o=s.object(a);
    if(first>o.pages.size() || count>o.pages.size()-first || count>s.options.prefetch_budget)throw std::invalid_argument("prefetch range/budget");
    std::size_t done=0;
    for(std::size_t i=first;i<first+count;++i) {
        const auto page=o.pages[i];
        if(std::any_of(s.frames.begin(),s.frames.end(),[&](const auto& f){return f.page==page;}))continue;
        // No speculative dirty writeback. Clean demand eviction is opt-in.
        if(s.backing[page].zero || s.leases.size()>=s.options.max_pins ||
           std::none_of(s.frames.begin(),s.frames.end(),[&](const auto& f){return !f.pins && !f.dirty && (f.page==absent || f.prefetched || s.options.prefetch_evict_clean);})) {++s.stats.prefetch_skipped;continue;}
        const auto lease=pin_impl(a,i,PageAccess::Read,true);unpin(lease);++done;++s.stats.prefetch_fills;
    }
    return done;
}
std::optional<PageLease> CapacityStore::pin_prefetched(Allocation a,std::size_t index) {
    auto& s=*impl_;const auto& object=s.object(a);
    if(index>=object.pages.size())throw std::invalid_argument("prefetched page range");
    if(s.leases.size()>=s.options.max_pins)return std::nullopt;
    const auto page=object.pages[index];
    if(std::none_of(s.frames.begin(),s.frames.end(),[&](const auto& f){return f.page==page && f.prefetched && !f.writer;}))return std::nullopt;
    return pin_impl(a,index,PageAccess::Read,true);
}
std::size_t CapacityStore::maintain(double gpu_watts) {
    auto& s=*impl_;s.current();
    if(!std::isfinite(gpu_watts) || gpu_watts<0)throw std::invalid_argument("invalid GPU power sample");
    if(!s.options.environment.enabled)return 0;
    s.gpu_watts=gpu_watts;
    try {
        const auto scan=[&]() {
            s.observe();s.stats.refresh_pending=0;s.stats.refresh_blocked=0;
            std::size_t selected=absent;double oldest=-1;
            for(std::size_t p=0;p<s.backing.size();++p) {
                auto& entry=s.backing[p];if(!entry.owner || entry.zero)continue;
                const auto age=s.environment.statistics().exposure-entry.programmed;
                if(age>=1 && !entry.expired){++s.stats.retention_expirations;entry.expired=true;}
                if(age<s.options.environment.refresh_fraction)continue;
                ++s.stats.refresh_pending;
                if(std::any_of(s.frames.begin(),s.frames.end(),[&](const auto& f){return f.page==p && f.pins;})){++s.stats.refresh_blocked;continue;}
                if(age>oldest){oldest=age;selected=p;}
            }
            return selected;
        };
        std::size_t done=0;
        for(;done<s.options.environment.refresh_budget;++done) {
            const auto page=scan();if(page==absent)break;
            const auto address=s.logical_base+page*4096;
            const auto read=s.shared?s.shared->read_media(address,4096):s.backend->read(address,4096);
            if(read.status!=Status::Ok)throw std::runtime_error("refresh read failed");
            s.stats.refresh_read_bytes+=4096;s.observe();
            auto it=std::find_if(s.frames.begin(),s.frames.end(),[&](const auto& f){return f.page==page;});
            if(it!=s.frames.end()){it->dirty=true;s.writeback(*it);}
            else {Impl::Frame frame;frame.page=page;frame.data=s.bytes(page);frame.dirty=true;s.writeback(frame);}
            ++s.stats.refreshes;
        }
        scan();return done;
    }catch(...){s.poison();throw;}
}
CapacityStats CapacityStore::statistics() const {
    auto& s=*impl_;s.current(true);auto stats=s.stats;stats.backend=s.media_stats();stats.pins=s.leases.size();stats.environment=s.environment.statistics();
    for(const auto& p:s.backing)if(p.owner)++stats.allocated_pages;
    for(const auto& f:s.frames)if(f.page!=absent){++stats.resident_frames;if(f.dirty)++stats.dirty_frames;}return stats;
}
std::uint64_t CapacityStore::model_time_ps() const {impl_->current();return impl_->now();}
void CapacityStore::abort(){impl_->current(true);impl_->poison();}
}
