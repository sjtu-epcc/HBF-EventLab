#pragma once
#include "hbf_eventlab/interfaces/assembly_storage.hpp"
#include <map>

namespace hbf_eventlab {
// BASELINE: one full data slot per pending DLU, retained through program termination.
// Host CPU map costs are NOT simulated SRAM latency or area.
class RetainedAssemblyStorage final : public IAssemblyStorage {
public:
    explicit RetainedAssemblyStorage(std::size_t capacity) : capacity_(capacity) {
        if (!capacity) throw std::invalid_argument("zero assembly capacity");
    }
    std::size_t capacity() const override { return capacity_; }
    std::size_t used() const { return pages_.size(); }
    AssemblySlotId allocate() override {
        if (pages_.size() == capacity_) throw std::runtime_error("assembly storage exhausted");
        const AssemblySlotId id{checked_add(next_, 1)};
        pages_.emplace(id, PageData{});
        next_ = id.value;
        return id;
    }
    void write(AssemblySlotId id, std::uint32_t slot, DataToken data) override { pages_.at(id).at(slot) = data; }
    const PageData& page(AssemblySlotId id) const override { return pages_.at(id); }
    void release(AssemblySlotId id) override {
        if (!pages_.erase(id)) throw std::logic_error("unknown assembly slot");
    }
private:
    std::size_t capacity_;
    std::uint64_t next_ = 0;
    std::map<AssemblySlotId, PageData> pages_;
};
} // namespace hbf_eventlab
