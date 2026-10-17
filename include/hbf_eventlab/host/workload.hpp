#pragma once
#include "hbf_eventlab/interfaces/host_port.hpp"
#include <string>
namespace hbf_eventlab {
enum class ReadHintMode { AllRegular, TailBatch };
enum class WriteCommandMode { Fragment64, Burst4096 };
struct GlobalRequest {
    GlobalRequestId id;
    Op op;
    GlobalByteAddress address;
    std::uint64_t bytes;
    std::uint64_t content=0; // immutable Host content identity, scoped to logical block
    std::optional<ReadGroup> group;
    std::vector<GlobalRequestId> after;
    Tick not_before{};
    std::optional<std::uint64_t> expected_content;
};
struct ContentKey {
    LogicalBlockKey block;
    std::uint64_t content;
    friend bool operator<(const ContentKey& a,const ContentKey& b) {return std::tie(a.block,a.content)<std::tie(b.block,b.content);}
};
struct Fragment {
    LogicalWorkId id;
    GlobalRequestId parent;
    GlobalByteAddress global;
    ChannelAddress address;
    LogicalNandAddress logical;
    DataToken data;
};
struct WorkResult {
    GlobalRequestId id;
    LogicalResult state=LogicalResult::Pending;
    std::size_t remaining=0,failed=0;
    std::string reason;
};
// Symbolic deterministic data, not a byte payload or hash-based correctness proof.
inline DataToken token_for(std::uint64_t content,GlobalByteAddress a) {
    return DataToken{(content*0x9e3779b97f4a7c15ULL)^(a.value/host_unit_bytes)};
}
struct HostConfiguration {
    std::string send_policy="fill_open"; // strategy selection; API injection records "custom"
    void validate() const;
    WriteCommandMode writes=WriteCommandMode::Fragment64;
    ReadHintMode hints=ReadHintMode::TailBatch;
    std::size_t max_open_dlus_per_channel=2;
    std::uint32_t max_replay_passes=3;
    std::size_t retained_fragments=65536; // 4 MiB logical Host storage; 0 deliberately disables retention
    std::size_t max_logical_fragments=1048576;
    // Host policies, not OCP requirements. Zero retains fail-on-error behavior.
    std::uint32_t max_pending_retries=3; // additional attempts per refused command
    std::uint32_t max_timeout_retries=3; // additional whole-DLU assembly rounds
    Tick pending_retry_delay{1000000}; // positive bounded fallback; progress can wake earlier
};
} // namespace hbf_eventlab

