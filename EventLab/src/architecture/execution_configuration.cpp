#include "hbf_eventlab/architecture/execution_configuration.hpp"
namespace hbf_eventlab {
void ExecutionConfiguration::validate(const StaticDeviceDescription& d) const {
    const auto& speed=ucie_speed_profile(ucie.speed_grade);
    if(d.geometry().host_channels>speed.max_modules)
        throw std::invalid_argument("device.geometry.host_channels: exceeds selected UCIe speed grade module limit");
    if(d.geometry().dies_per_channel>speed.max_stack_height)
        throw std::invalid_argument("device.geometry.dies_per_channel: exceeds selected UCIe speed grade stack limit");
    if(d.capabilities().read_burst)
        throw std::invalid_argument("device.capabilities.read_burst: current DeviceDriver supports only 64B reads");
    if(!media_timing.sense.value) throw std::invalid_argument("execution.media_timing.sense_ps: must be positive");
    if(!media_timing.program.value) throw std::invalid_argument("execution.media_timing.program_ps: must be positive");
    if(!media_timing.erase.value) throw std::invalid_argument("execution.media_timing.erase_ps: must be positive");
    if(!media_timing.cache_access.value) throw std::invalid_argument("execution.media_timing.cache_access_ps: must be positive");
    if(!ucie.overhead.request_control_bytes) throw std::invalid_argument("execution.ucie.request_control_bytes: must be positive");
    if(!ucie.overhead.response_control_bytes) throw std::invalid_argument("execution.ucie.response_control_bytes: must be positive");
    if(!buffers.ingress.entries) throw std::invalid_argument("execution.buffers.ingress_entries: must be positive");
    if(!buffers.egress.entries) throw std::invalid_argument("execution.buffers.egress_entries: must be positive");
    if(buffers.egress.payload_bytes<host_unit_bytes)
        throw std::invalid_argument("execution.buffers.egress_payload_bytes: must fit one 64B read result");
    if(buffers.ingress.payload_bytes<(d.capabilities().write_burst?dlu_bytes:host_unit_bytes))
        throw std::invalid_argument("execution.buffers.ingress_payload_bytes: must fit largest advertised write transaction");
    try {
        const auto largest_write=d.capabilities().write_burst?dlu_bytes:host_unit_bytes;
        const auto request=ucie.serialization(checked_add(ucie.overhead.request_control_bytes,largest_write));
        const auto response=ucie.serialization(checked_add(ucie.overhead.response_control_bytes,host_unit_bytes));
        (void)checked_add(request.value,ucie.overhead.request_fixed_latency.value);
        (void)checked_add(response.value,ucie.overhead.response_fixed_latency.value);
    } catch(const std::overflow_error&) {
        throw std::invalid_argument("execution.ucie: service bytes or transfer duration overflow");
    }
}
Tick UcieExecutionConfiguration::serialization(std::uint64_t bytes) const {
    const auto bandwidth=effective_gbytes_per_second();
    const auto ps=checked_mul(bytes,1000);
    return Tick{ps/bandwidth+(ps%bandwidth!=0)};
}
}
