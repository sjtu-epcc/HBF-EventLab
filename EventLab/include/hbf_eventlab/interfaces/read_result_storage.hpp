#pragma once
#include "hbf_eventlab/common/types.hpp"
#include <optional>
#include <utility>

namespace hbf_eventlab {
using ReadResultId = Quantity<struct ReadResultTag>;
// Explicit bounded owner for one symbolic 64B result. BankCache and EgressBuffer
// supply production storage. A DataToken consumes its logical hardware capacity.
class IReadResultStorage {
public:
    virtual ~IReadResultStorage() = default;
    virtual std::optional<ReadResultId> reserve() = 0;
    virtual std::optional<ReadResultId> reserve(ChannelId) { return reserve(); }
    virtual void store(ReadResultId, DataToken) = 0;
    virtual DataToken load(ReadResultId) const = 0;
    virtual void release(ReadResultId) noexcept = 0;
    virtual std::size_t used_bytes() const = 0;
    virtual std::size_t capacity_bytes() const = 0;
    // Default is one shared result pool across channels. Channel-local owners
    // override both the wake scope and the actual entry+byte availability query.
    virtual ChannelId space_channel(ChannelId) const { return 0; }
    virtual bool can_reserve(ChannelId) const { return capacity_bytes()-used_bytes()>=host_unit_bytes; }
};
class ReadResultLease {
public:
    ReadResultLease(IReadResultStorage& store, ReadResultId id) : store_(&store), id_(id) {}
    ReadResultLease(const ReadResultLease&) = delete;
    ReadResultLease& operator=(const ReadResultLease&) = delete;
    ReadResultLease(ReadResultLease&& other) noexcept : store_(std::exchange(other.store_,nullptr)),id_(other.id_) {}
    ReadResultLease& operator=(ReadResultLease&& other) noexcept {
        if (this != &other) { reset(); store_=std::exchange(other.store_,nullptr); id_=other.id_; } return *this;
    }
    ~ReadResultLease() { reset(); }
    void store(DataToken data) { store_->store(id_,data); }
    DataToken load() const { return store_->load(id_); }
    bool belongs_to(const IReadResultStorage& s) const { return store_ == &s; }
    ReadResultId id() const { return id_; }
private:
    void reset() noexcept { if (store_) { store_->release(id_); store_=nullptr; } }
    IReadResultStorage* store_;
    ReadResultId id_;
};
} // namespace hbf_eventlab
