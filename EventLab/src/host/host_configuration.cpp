#include "hbf_eventlab/host/workload.hpp"
namespace hbf_eventlab {
void HostConfiguration::validate() const {
    if(!max_open_dlus_per_channel) throw std::invalid_argument("host.max_open_dlus_per_channel: must be positive");
    // Both guards may be disabled explicitly. Monotonic identities still use
    // checked arithmetic, and active work is never silently discarded.
    if(max_pending_retries && !pending_retry_delay.value)
        throw std::invalid_argument("host.retry.pending_delay_ps: must be positive when Pending retries are enabled");
    if(writes!=WriteCommandMode::Fragment64 && writes!=WriteCommandMode::Burst4096)
        throw std::invalid_argument("host.write_mode: unsupported value");
    if(hints!=ReadHintMode::AllRegular && hints!=ReadHintMode::TailBatch)
        throw std::invalid_argument("host.read_hint_mode: unsupported value");
}
}
