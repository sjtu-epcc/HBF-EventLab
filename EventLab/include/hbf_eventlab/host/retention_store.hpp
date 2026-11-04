#pragma once
#include "hbf_eventlab/host/workload.hpp"
#include <deque>
#include <set>
namespace hbf_eventlab {
struct RetainedKey {
    ContentKey content;
    PageId page;
    std::uint32_t slot;
    friend bool operator<(const RetainedKey&a,const RetainedKey&b) {
        return std::tie(a.content,a.page,a.slot)<std::tie(b.content,b.page,b.slot);
    }
};
struct RetainedFragment {GlobalByteAddress global;DataToken token;};
class WriteRetentionStore {
public:
    explicit WriteRetentionStore(std::size_t limit):limit_(limit) {}
    void save(ContentKey,const Fragment&);
    std::optional<RetainedFragment> find(ContentKey,PageId,std::uint32_t) const;
    bool pin_prefix(ContentKey,PageId);
    void unpin(ContentKey);
    std::size_t size() const {return data_.size();}
    std::size_t capacity() const {return limit_;}
private:
    std::size_t limit_;
    std::map<RetainedKey,RetainedFragment> data_;
    std::deque<RetainedKey> fifo_;
    std::set<RetainedKey> pinned_;
};
} // namespace hbf_eventlab

