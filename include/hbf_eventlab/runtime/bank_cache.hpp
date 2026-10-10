#pragma once
#include "hbf_eventlab/runtime/device_runtime_state.hpp"
#include "hbf_eventlab/interfaces/read_result_storage.hpp"
#include "hbf_eventlab/interfaces/assembly_storage.hpp"
#include <array>
#include <map>

namespace hbf_eventlab {
struct CacheFill { BankKey bank; std::uint32_t slot; Generation generation; };
// Two 4KiB slots per physical Bank. Invalidating a tag never destroys pinned results.
class BankCache final : public IReadResultStorage {
public:
    bool contains(PhysicalNandAddress) const;
    bool can_fill(BankKey) const;
    std::optional<CacheFill> begin_fill(PhysicalNandAddress,const PageData*);
    void finish_fill(CacheFill);
    void cancel_fill(CacheFill);
    std::optional<ReadResultLease> pin(PhysicalNandAddress);
    ReadResultLease pin_fill(CacheFill,std::uint32_t byte_offset);
    void invalidate(PhysicalBlockKey) noexcept;
    void invalidate_page(PhysicalNandAddress) noexcept;
    void invalidate_all() noexcept;
    std::size_t protected_slots() const { return protected_; }
    std::size_t filling_slots() const { return filling_; }
    // Unaddressed reservations are forbidden. pin()/pin_fill() establish ownership.
    std::optional<ReadResultId> reserve() override { throw std::logic_error("Cache reservation needs an address"); }
    void store(ReadResultId,DataToken) override;
    DataToken load(ReadResultId) const override;
    void release(ReadResultId) noexcept override;
    std::size_t used_bytes() const override; // physical slot storage, shared by any number of result handles
    std::size_t capacity_bytes() const override { return banks_.size()*2*dlu_bytes; }
private:
    struct Slot {
        std::optional<PhysicalNandAddress> tag;
        PageData data{};
        Generation generation{};
        std::uint64_t age=0;
        std::size_t references=0;
        bool filling=false, fill_valid=false;
    };
    struct Ref { CacheFill slot; std::uint32_t fragment; };
    Slot& get(CacheFill);
    const Slot& get(CacheFill) const;
    ReadResultLease reference(CacheFill,std::uint32_t);
    std::map<BankKey,std::array<Slot,2>> banks_;
    std::map<ReadResultId,Ref> refs_;
    std::uint64_t next_=0,age_=0;
    std::size_t protected_=0,filling_=0;
};
} // namespace hbf_eventlab

