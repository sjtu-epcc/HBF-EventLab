#include "hbflab/kv.hpp"
#include <atomic>
#include <map>
#include <thread>

namespace hbflab {
namespace {std::atomic<std::uint64_t> next_cache{1};}
struct KvCache::Impl {
    struct Pair {Allocation key,value;};
    struct Sequence {std::size_t length=0;std::vector<std::vector<Pair>> blocks;
        std::uint64_t transaction=0;std::size_t pending=0;std::vector<bool> ready;};
    CapacityStore& store;
    KvOptions options;
    std::size_t row,block_bytes,block_pages;
    std::map<std::uint64_t,Sequence> sequences;
    const std::uint64_t identity=next_cache.fetch_add(1);
    std::uint64_t serial=0;
    const std::thread::id thread=std::this_thread::get_id();
    bool failed=false;
    Impl(CapacityStore& s,KvOptions o):store(s),options(o) {
        if(!o.layers || o.layers>256 || !o.heads || !o.head_dimension ||
           (o.element_bytes!=1 && o.element_bytes!=2 && o.element_bytes!=4 && o.element_bytes!=8) ||
           !o.tokens_per_block || !o.max_tokens || !o.max_sequences || o.max_sequences>65536)
            throw std::invalid_argument("invalid KV layout");
        row=hbf_eventlab::checked_mul(hbf_eventlab::checked_mul(o.heads,o.head_dimension),o.element_bytes);
        block_bytes=hbf_eventlab::checked_mul(row,o.tokens_per_block);
        block_pages=hbf_eventlab::checked_mul(hbf_eventlab::checked_add(block_bytes,4095)/4096,o.layers*2);
        hbf_eventlab::checked_mul(row,o.layers);
    }
    void current() const {
        if(thread!=std::this_thread::get_id())throw std::logic_error("KV host owner changed");
        if(failed || store.statistics().poisoned)throw std::runtime_error("KV cache poisoned by incomplete operation");
    }
    Sequence& sequence(KvSequence h) {
        current();const auto it=sequences.find(h.value);
        if(h.cache!=identity || it==sequences.end())throw std::invalid_argument("foreign/stale KV sequence");return it->second;
    }
    Sequence& transaction(KvAppend h) {
        auto& q=sequence({h.cache,h.sequence});
        if(!h.value || h.value!=q.transaction)throw std::invalid_argument("stale KV append transaction");
        return q;
    }
    void idle(const Sequence& q) const {if(q.transaction)throw CapacityBusy("KV append transaction active");}
    void unpinned(const Sequence& s) const {
        for(const auto& b:s.blocks)for(const auto& p:b)
            if(store.pin_count(p.key) || store.pin_count(p.value))throw CapacityBusy("KV mutation requires released sequence leases");
    }
    void free_block(const std::vector<Pair>& b){for(const auto& p:b){store.release(p.key);store.release(p.value);}}
};
KvCache::KvCache(CapacityStore& store,KvOptions o):impl_(std::make_unique<Impl>(store,o)){}
KvCache::~KvCache(){
    // Teardown discards owned data, never performs implicit media writes. Callers
    // must finish leases first; a pinned allocation is preserved until store teardown.
    for(const auto& s:impl_->sequences)for(const auto& b:s.second.blocks)for(const auto& p:b) {
        try {impl_->store.release(p.key);}catch(...){}
        try {impl_->store.release(p.value);}catch(...){}
    }
}
KvSequence KvCache::create() {
    auto& s=*impl_;s.current();if(s.sequences.size()>=s.options.max_sequences)throw CapacityBusy("KV sequence budget exhausted");
    s.serial=hbf_eventlab::checked_add(s.serial,1);s.sequences.emplace(s.serial,Impl::Sequence{});return {s.identity,s.serial};
}
void KvCache::release(KvSequence h) {
    auto& s=*impl_;const auto& sequence=s.sequence(h);s.idle(sequence);s.unpinned(sequence);
    for(const auto& b:sequence.blocks)s.free_block(b);s.sequences.erase(h.value);
}
void KvCache::truncate(KvSequence h,std::size_t tokens) {
    auto& s=*impl_;auto& sequence=s.sequence(h);s.idle(sequence);s.unpinned(sequence);
    if(tokens>sequence.length)throw std::invalid_argument("KV truncate cannot grow sequence");
    const auto count=tokens/s.options.tokens_per_block+(tokens%s.options.tokens_per_block!=0);
    while(sequence.blocks.size()>count){s.free_block(sequence.blocks.back());sequence.blocks.pop_back();}sequence.length=tokens;
}
void KvCache::append(KvSequence h,const void* keys,const void* values,std::size_t bytes) {
    auto& s=*impl_;auto& sequence=s.sequence(h);s.idle(sequence);s.unpinned(sequence);
    if(!keys || !values || bytes!=s.row*s.options.layers || sequence.length>=s.options.max_tokens)
        throw std::invalid_argument("KV append requires one complete K/V token within token limit");
    // Avoid partial mutation caused by external pins exhausting the frame pool.
    if(s.store.statistics().pins)throw CapacityBusy("KV append requires an idle frame pool");
    const auto block=sequence.length/s.options.tokens_per_block;
    if(block==sequence.blocks.size()) {
        if(s.store.free_pages()<s.block_pages)throw CapacityBusy("KV block exceeds remaining logical capacity");
        std::vector<Impl::Pair> allocated;allocated.reserve(s.options.layers);
        sequence.blocks.reserve(sequence.blocks.size()+1);
        try {
            for(std::size_t layer=0;layer<s.options.layers;++layer) {
                const auto key=s.store.allocate(s.block_bytes);
                try {const auto value=s.store.allocate(s.block_bytes);allocated.push_back({key,value});}
                catch(...){s.store.release(key);throw;}
            }
        } catch(...){s.free_block(allocated);throw;}
        sequence.blocks.push_back(std::move(allocated));
    }
    try {
        const auto offset=(sequence.length%s.options.tokens_per_block)*s.row;
        for(std::size_t layer=0;layer<s.options.layers;++layer) {
            const auto& p=sequence.blocks[block][layer];
            s.store.write(p.key,offset,static_cast<const unsigned char*>(keys)+layer*s.row,s.row);
            s.store.write(p.value,offset,static_cast<const unsigned char*>(values)+layer*s.row,s.row);
        }
        ++sequence.length;
    } catch(...){s.failed=true;throw;}
}
KvAppend KvCache::begin_append(KvSequence h,std::size_t tokens) {
    auto& s=*impl_;auto& q=s.sequence(h);s.idle(q);s.unpinned(q);
    if(!tokens || tokens>s.options.max_tokens-q.length)throw std::invalid_argument("KV batch token limit");
    if(s.store.statistics().pins)throw CapacityBusy("KV append requires idle frame pool");
    hbf_eventlab::checked_mul(tokens,s.row);
    const auto total=q.length+tokens;
    const auto needed=total/s.options.tokens_per_block+(total%s.options.tokens_per_block!=0);
    if(needed-q.blocks.size()>s.store.free_pages()/s.block_pages)throw CapacityBusy("KV batch capacity exhausted");
    const auto old=q.blocks.size();q.blocks.reserve(needed);q.ready.assign(s.options.layers,false);
    try {
        while(q.blocks.size()<needed) {
            std::vector<Impl::Pair> b;b.reserve(s.options.layers);
            try {for(std::size_t i=0;i<s.options.layers;++i){auto k=s.store.allocate(s.block_bytes);
                try{auto v=s.store.allocate(s.block_bytes);b.push_back({k,v});}catch(...){s.store.release(k);throw;}}}
            catch(...){s.free_block(b);throw;}
            q.blocks.push_back(std::move(b));
        }
    } catch(...){while(q.blocks.size()>old){s.free_block(q.blocks.back());q.blocks.pop_back();}throw;}
    s.serial=hbf_eventlab::checked_add(s.serial,1);q.transaction=s.serial;q.pending=tokens;
    return {s.identity,h.value,q.transaction};
}
void KvCache::write_layer(KvAppend h,std::size_t layer,const void* keys,const void* values,std::size_t bytes) {
    auto& s=*impl_;auto& q=s.transaction(h);
    if(layer>=s.options.layers || q.ready[layer] || !keys || !values || bytes!=q.pending*s.row)
        throw std::invalid_argument("invalid or duplicate KV layer submission");
    s.unpinned(q);if(s.store.statistics().pins)throw CapacityBusy("KV layer write requires idle frame pool");
    try {
        for(std::size_t i=0;i<q.pending;++i) {
            const auto token=q.length+i;const auto& p=q.blocks[token/s.options.tokens_per_block][layer];
            const auto offset=(token%s.options.tokens_per_block)*s.row;
            s.store.write(p.key,offset,static_cast<const unsigned char*>(keys)+i*s.row,s.row);
            s.store.write(p.value,offset,static_cast<const unsigned char*>(values)+i*s.row,s.row);
        }
        q.ready[layer]=true;
    }catch(...){s.failed=true;throw;}
}
KvSlice KvCache::locate_layer(KvAppend h,std::size_t layer,std::size_t token,KvKind kind) const {
    auto& s=*impl_;const auto& q=s.transaction(h);
    if(layer>=s.options.layers || token>=q.length+(q.ready[layer]?q.pending:0) ||
       (kind!=KvKind::Key && kind!=KvKind::Value))throw std::invalid_argument("KV layer is not published");
    const auto& p=q.blocks[token/s.options.tokens_per_block][layer];
    return {kind==KvKind::Key?p.key:p.value,(token%s.options.tokens_per_block)*s.row,s.row};
}
void KvCache::commit(KvAppend h) {
    auto& s=*impl_;auto& q=s.transaction(h);s.unpinned(q);
    for(bool ready:q.ready)if(!ready)throw CapacityBusy("KV batch has unpublished layers");
    q.length+=q.pending;q.pending=0;q.transaction=0;q.ready.clear();
}
void KvCache::abort(KvAppend h){auto& s=*impl_;s.transaction(h);s.failed=true;s.store.abort();}
KvSlice KvCache::locate(KvSequence h,std::size_t layer,std::size_t token,KvKind kind) const {
    auto& s=*impl_;const auto& sequence=s.sequence(h);
    if(layer>=s.options.layers || token>=sequence.length || (kind!=KvKind::Key && kind!=KvKind::Value))
        throw std::invalid_argument("KV layer/token/kind out of range");
    const auto& p=sequence.blocks[token/s.options.tokens_per_block][layer];
    return {kind==KvKind::Key?p.key:p.value,(token%s.options.tokens_per_block)*s.row,s.row};
}
void KvCache::read(KvSequence h,std::size_t layer,std::size_t token,KvKind kind,void* out,std::size_t bytes) {
    const auto slice=locate(h,layer,token,kind);if(bytes!=slice.bytes || !out)throw std::invalid_argument("KV read row size");
    impl_->store.read(slice.allocation,slice.offset,out,bytes);
}
void KvCache::flush(KvSequence h){auto& s=*impl_;const auto& sequence=s.sequence(h);s.unpinned(sequence);for(const auto& b:sequence.blocks)for(const auto& p:b){s.store.flush(p.key);s.store.flush(p.value);}}
std::size_t KvCache::length(KvSequence h) const{return impl_->sequence(h).length;}
std::size_t KvCache::row_bytes() const{impl_->current();return impl_->row;}
bool KvCache::poisoned() const{return impl_->failed || impl_->store.statistics().poisoned;}
}
