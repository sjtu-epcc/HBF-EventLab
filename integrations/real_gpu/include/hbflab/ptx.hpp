#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hbflab {
inline constexpr std::uint32_t ptx_frontend_version = 2;
inline constexpr std::uint32_t async_ptx_frontend_version = 4;
struct PtxAccess {
    unsigned source_line = 0, instruction = 0, bytes = 0;
    std::string opcode, predicate;
    bool store = false;
};
struct PtxEntry {
    std::string name;
    std::vector<unsigned> parameter_bytes;
    unsigned loads = 0, stores = 0;
    std::vector<PtxAccess> accesses;
};
struct RewrittenPtx {
    std::string text;
    std::vector<PtxEntry> entries;
    unsigned target_sm = 0;
};
// Strict, bounded straight-line PTX subset. Unsupported input throws with a line.
RewrittenPtx rewrite_ptx(std::string_view source);
RewrittenPtx rewrite_async_ptx(std::string_view source);
std::string sha256(std::string_view bytes);
}
