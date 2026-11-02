#include "hbflab/platform.hpp"
#include <cstdint>
#include <cstring>

int main() {
    hbflab::platform::set_environment("HBFLAB_PLATFORM_TEST", "configured");
    const auto* value = std::getenv("HBFLAB_PLATFORM_TEST");
    if (!value || std::strcmp(value, "configured")) return 1;
    void* buffer = hbflab::platform::aligned_allocate(8192, 64);
    if (!buffer || reinterpret_cast<std::uintptr_t>(buffer) % 64) return 2;
    std::memset(buffer, 0x5a, 8192);
    hbflab::platform::aligned_release(buffer);
    return hbflab::platform::available_host_memory() > 0 ? 0 : 3;
}
