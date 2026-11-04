#pragma once
#include "hbf_eventlab/interfaces/read_result_storage.hpp"
#include <map>

namespace hbf_eventlab::test_support {
// Stage-one component harness ONLY. Size is explicit, not a hardware buffer claim.
class BoundedReadResults final : public IReadResultStorage {
public:
    explicit BoundedReadResults(std::size_t slots) : slots_(slots) {}
    std::optional<ReadResultId> reserve() override {
        if (data_.size() == slots_) return {};
        const ReadResultId id{checked_add(next_,1)};
        data_.emplace(id,std::nullopt); next_=id.value; return id;
    }
    void store(ReadResultId id, DataToken value) override {
        auto& slot = data_.at(id); if (slot) throw std::logic_error("result already stored"); slot=value;
    }
    DataToken load(ReadResultId id) const override { return data_.at(id).value(); }
    void release(ReadResultId id) noexcept override { data_.erase(id); }
    std::size_t used_bytes() const override { return data_.size()*host_unit_bytes; }
    std::size_t capacity_bytes() const override { return slots_*host_unit_bytes; }
private:
    std::size_t slots_;
    std::uint64_t next_ = 0;
    std::map<ReadResultId,std::optional<DataToken>> data_;
};
} // namespace hbf_eventlab::test_support
