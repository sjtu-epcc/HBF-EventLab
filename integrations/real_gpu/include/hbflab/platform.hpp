#pragma once
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#else
#include <unistd.h>
#endif

namespace hbflab::platform {
inline void set_environment(const char* name, const char* value) {
#ifdef _WIN32
    const int result = _putenv_s(name, value);
#else
    const int result = setenv(name, value, 1);
#endif
    if (result != 0) throw std::runtime_error("Cannot set process environment");
}
inline void* aligned_allocate(std::size_t bytes, std::size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(bytes, alignment);
#else
    void* pointer = nullptr;
    return posix_memalign(&pointer, alignment, bytes) == 0 ? pointer : nullptr;
#endif
}
inline void aligned_release(void* pointer) {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
inline std::uint64_t available_host_memory() {
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) throw std::runtime_error("Cannot query host memory");
    return memory.ullAvailPhys;
#else
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long bytes = sysconf(_SC_PAGESIZE);
    if (pages < 0 || bytes <= 0) throw std::runtime_error("Cannot query available host memory");
    return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(bytes);
#endif
}
}
