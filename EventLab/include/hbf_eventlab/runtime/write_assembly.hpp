#pragma once
#include "hbf_eventlab/runtime/command_admission.hpp"
#include "hbf_eventlab/interfaces/assembly_storage.hpp"
#include "hbf_eventlab/common/write_observation.hpp"
#include <bitset>
#include <functional>
#include <optional>

namespace hbf_eventlab {
struct PendingWriteContext {
    DluKey key;
    Generation generation;
    Tick first_arrival, deadline;
    WritePhase phase = WritePhase::Collecting;
    std::bitset<fragments_per_dlu> received;
    AssemblySlotId data_slot;
    std::vector<CommandId> awaiting_completion;
};
struct AssemblyResult {
    WriteStatus status;
    Generation generation;
    bool opened, became_ready;
    Tick deadline;
};
enum class ForwardKind { NoPendingWrite, MissingFragment, Available };
struct ForwardResult { ForwardKind kind; DataToken data; };
struct DluCompletion {
    DluKey key;
    Generation generation;
    WriteStatus status;
    std::uint8_t additional_info;
    std::vector<CommandId> commands; // Deduplicated: one 4KiB burst -> one response.
};
class WriteAssembly {
public:
    // Store must outlive this table and be dedicated to it. Fully provisioned baseline.
    WriteAssembly(const StaticDeviceDescription&, IAssemblyStorage&);
    WriteAssembly(const WriteAssembly&) = delete;
    WriteAssembly& operator=(const WriteAssembly&) = delete;
    ~WriteAssembly();
    // Attach once before the first context. Synchronous observation only: no
    // reentry, state mutation, or scheduled events. Teardown is not retirement.
    void set_observer(std::function<void(const AssemblyObservation&)>);
    // Caller has validated command and Host block order BEFORE mutation here.
    AssemblyResult accept(const CommandFragment&, DataToken, Tick now);
    // Preflight the ENTIRE normalized command before any fragment mutates the table.
    WriteStatus check_command(const std::vector<CommandFragment>&, Tick now) const;
    ForwardResult forward(DluKey, std::uint32_t slot) const;
    const PendingWriteContext* find(DluKey) const;
    std::size_t pending(ChannelId) const;
    void queue_program(DluKey, Generation);
    void start_program(DluKey, Generation);
    // Sink atomically commits media/cache state before buffer removal. It must not
    // re-enter this table; if it throws it must leave its own media unchanged.
    DluCompletion commit_program(DluKey, Generation, const std::function<void(const PageData&)>& sink);
    DluCompletion fail_program(DluKey, Generation);
    DluCompletion fail_prior_program(DluKey, Generation);
    // Safe to call from an old event: generation mismatch/Ready/Programming -> no-op.
    std::optional<DluCompletion> expire(DluKey, Generation, Tick now);
private:
    PendingWriteContext& require(DluKey, Generation);
    DluCompletion retire(DluKey, WriteStatus, std::uint8_t info);
    void publish(AssemblyChange,const PendingWriteContext&,std::optional<CommandId> = {});
    std::uint64_t channel_dlus_, next_generation_ = 0;
    std::uint32_t limit_;
    Tick timeout_;
    IAssemblyStorage& storage_;
    std::vector<std::size_t> counts_;
    std::map<DluKey, PendingWriteContext> contexts_;
    std::function<void(const AssemblyObservation&)> observer_;
};
} // namespace hbf_eventlab
