#pragma once
#include "hbf_eventlab/architecture/execution_configuration.hpp"
#include "hbf_eventlab/interfaces/read_result_storage.hpp"
#include <map>
namespace hbf_eventlab {
class BufferBudget {
public:
    explicit BufferBudget(BufferCapacity c):capacity_(c) {}
    bool fits(std::size_t bytes) const { return entries_<capacity_.entries && bytes<=capacity_.payload_bytes-bytes_; }
    bool reserve(std::size_t bytes) { if(!fits(bytes)) return false; ++entries_; bytes_+=bytes; return true; }
    void release(std::size_t bytes) {
        if(!entries_ || bytes>bytes_) throw std::logic_error("buffer accounting underflow"); --entries_; bytes_-=bytes;
    }
    std::size_t entries() const { return entries_; }
    std::size_t bytes() const { return bytes_; }
    BufferCapacity capacity() const { return capacity_; }
private:
    BufferCapacity capacity_;
    std::size_t entries_=0,bytes_=0;
};
// Each entry is owned either by a Parent's ReadResultLease or by a metadata response.
// It remains allocated throughout response transmission, not just until link start.
class EgressBuffer final : public IReadResultStorage {
public:
    EgressBuffer(std::size_t channels,BufferCapacity c):budget_(channels,BufferBudget(c)) {}
    std::optional<ReadResultId> reserve() override { return reserve(0); }
    std::optional<ReadResultId> reserve(ChannelId ch) override { return allocate(ch,host_unit_bytes); }
    std::optional<ReadResultId> reserve_metadata(ChannelId ch) { return allocate(ch,0); }
    void store(ReadResultId id,DataToken token) override { entries_.at(id).data=token; }
    DataToken load(ReadResultId id) const override {
        const auto& e=entries_.at(id); if(!e.data) throw std::logic_error("uninitialized Egress result"); return *e.data;
    }
    void release(ReadResultId id) noexcept override {
        const auto it=entries_.find(id); if(it==entries_.end()) return;
        budget_.at(it->second.channel).release(it->second.bytes); entries_.erase(it);
    }
    std::size_t used_bytes() const override { std::size_t n=0;for(const auto& b:budget_)n+=b.bytes();return n; }
    std::size_t capacity_bytes() const override { std::size_t n=0;for(const auto& b:budget_)n+=b.capacity().payload_bytes;return n; }
    const BufferBudget& budget(ChannelId ch) const { return budget_.at(ch); }
private:
    struct Entry { ChannelId channel; std::size_t bytes; std::optional<DataToken> data; };
    std::optional<ReadResultId> allocate(ChannelId ch,std::size_t bytes) {
        auto& b=budget_.at(ch); if(!b.fits(bytes)) return {};
        const ReadResultId id{checked_add(next_,1)};
        entries_.emplace(id,Entry{ch,bytes,{}}); b.reserve(bytes); next_=id.value; return id;
    }
    std::vector<BufferBudget> budget_;
    std::map<ReadResultId,Entry> entries_;
    std::uint64_t next_=0;
};
} // namespace hbf_eventlab

