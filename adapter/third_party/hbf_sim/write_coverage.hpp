#pragma once
// Adapted from HBF-Sim hbf/hbf_controller.h at 3415a3171d9312fac6a835bc1aa188f7aab685c0.
// Only surrounding GPU mask types and namespace are supplied here; coverage body is retained.
// Copyright notices and license: LICENSE and NOTICE.md in this directory.
#include <bitset>
#include <cstddef>
#include <vector>
namespace hbf_sim_import {
constexpr unsigned MAX_MEMORY_ACCESS_SIZE = 128;
using mem_access_byte_mask_t = std::bitset<MAX_MEMORY_ACCESS_SIZE>;
class hbf_write_coverage_t {
 public:
  hbf_write_coverage_t() : m_covered_bytes(0) {}
  explicit hbf_write_coverage_t(unsigned page_size)
      : m_covered(page_size, 0), m_covered_bytes(0) {}

  void reset(unsigned page_size) {
    m_covered.assign(page_size, 0);
    m_covered_bytes = 0;
  }

  // Returns false for a request that is not wholly contained in this page.
  // new_bytes receives only bytes not covered by an earlier request.
  bool add(unsigned long long page_offset, unsigned bytes,
           unsigned *new_bytes) {
    if (new_bytes != NULL) *new_bytes = 0;
    if (page_offset > m_covered.size() ||
        bytes > m_covered.size() - page_offset) {
      return false;
    }
    unsigned added = 0;
    for (unsigned long long i = page_offset;
         i < page_offset + bytes; ++i) {
      if (m_covered[(size_t)i] == 0) {
        m_covered[(size_t)i] = 1;
        added++;
      }
    }
    m_covered_bytes += added;
    if (new_bytes != NULL) *new_bytes = added;
    return true;
  }

  // Byte-mask bits are relative to the enclosing 128-byte memory-access
  // window, not necessarily to mem_fetch::get_addr(). Validate the complete
  // mask before mutating coverage so a cross-page request is atomic.
  bool add_mask(unsigned long long mask_page_offset,
                const mem_access_byte_mask_t &mask, unsigned *new_bytes) {
    if (new_bytes != NULL) *new_bytes = 0;
    for (unsigned i = 0; i < MAX_MEMORY_ACCESS_SIZE; ++i) {
      if (mask.test(i) && mask_page_offset + i >= m_covered.size())
        return false;
    }
    unsigned added = 0;
    for (unsigned i = 0; i < MAX_MEMORY_ACCESS_SIZE; ++i) {
      if (!mask.test(i)) continue;
      size_t offset = (size_t)(mask_page_offset + i);
      if (m_covered[offset] == 0) {
        m_covered[offset] = 1;
        ++added;
      }
    }
    m_covered_bytes += added;
    if (new_bytes != NULL) *new_bytes = added;
    return true;
  }

  unsigned covered_bytes() const { return m_covered_bytes; }
  unsigned page_size() const { return (unsigned)m_covered.size(); }
  bool full() const {
    return !m_covered.empty() && m_covered_bytes == m_covered.size();
  }

 private:
  std::vector<unsigned char> m_covered;
  unsigned m_covered_bytes;
};

} // namespace hbf_sim_import

