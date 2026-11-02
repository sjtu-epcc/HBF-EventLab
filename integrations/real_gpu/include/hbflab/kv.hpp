#pragma once
#include "hbflab/capacity.hpp"
namespace hbflab {
struct KvOptions {
    std::size_t layers=1,heads=1,head_dimension=64,element_bytes=2;
    std::size_t tokens_per_block=16,max_tokens=4096,max_sequences=16;
};
struct KvSequence {std::uint64_t cache=0,value=0;};
struct KvAppend {std::uint64_t cache=0,sequence=0,value=0;};
enum class KvKind { Key, Value };
struct KvSlice {Allocation allocation;std::size_t offset=0,bytes=0;};
// Layout: each block has separate K and V allocations per layer, each laid out
// [token_in_block][head][head_dimension][element_bytes]. Bytes are dtype-opaque.
class KvCache {
public:
    KvCache(CapacityStore&,KvOptions = {});
    ~KvCache(); // Discards owned unpinned allocations; finish all leases first.
    KvCache(const KvCache&)=delete;
    KvCache& operator=(const KvCache&)=delete;
    KvSequence create();
    void release(KvSequence);
    void truncate(KvSequence,std::size_t tokens);
    // K and V each contain [layer][head][head_dimension] for ONE token.
    // Publish length only after all layers complete. Errors after mutation poison cache.
    void append(KvSequence,const void* keys,const void* values,std::size_t bytes_each);
    // One batch transaction per sequence. A layer becomes visible only after
    // both K and V are written; sequence length publishes at commit.
    KvAppend begin_append(KvSequence,std::size_t tokens);
    void write_layer(KvAppend,std::size_t layer,const void* keys,const void* values,std::size_t bytes_each);
    KvSlice locate_layer(KvAppend,std::size_t layer,std::size_t token,KvKind) const;
    void commit(KvAppend);
    void abort(KvAppend); // Incomplete writes poison the cache; no rollback claim.
    void read(KvSequence,std::size_t layer,std::size_t token,KvKind,void*,std::size_t bytes);
    KvSlice locate(KvSequence,std::size_t layer,std::size_t token,KvKind) const;
    void flush(KvSequence);
    std::size_t length(KvSequence) const;
    std::size_t row_bytes() const;
    bool poisoned() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
