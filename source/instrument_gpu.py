from pathlib import Path

root = Path(__import__('sys').argv[1])
p = root / 'gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc'
s = p.read_text()
needle = '#include "gpu-sim.h"\n'
assert s.count(needle) == 1
s = s.replace(needle, needle + '''
#include <chrono>
namespace gpu_diag {
using clock = std::chrono::steady_clock;
enum Zone { total, hbf_bridge, hbf_partition, dram_other, l2, core, block_issue, count };
static const char *names[] = {"total", "hbf_bridge", "hbf_partition", "dram_other", "l2", "core", "block_issue"};
static unsigned long long ns[count] = {};
static unsigned long long calls = 0;
struct Timer {
  Zone zone;
  clock::time_point start;
  Timer(Zone z) : zone(z), start(clock::now()) {}
  ~Timer() { ns[zone] += std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count(); }
};
static void report(unsigned long long sim_cycle) {
  fprintf(stderr, "GPU_DIAG cycles=%llu sim_cycle=%llu", calls, sim_cycle);
  for (int i = 0; i < count; ++i) fprintf(stderr, " %s_ns=%llu", names[i], ns[i]);
  fprintf(stderr, "\\n"); fflush(stderr);
}
}
''')
needle = 'void gpgpu_sim::cycle() {\n  int clock_mask = next_clock_domain();'
assert s.count(needle) == 1
s = s.replace(needle, '''void gpgpu_sim::cycle() {
  gpu_diag::Timer total_timer(gpu_diag::total);
  if (++gpu_diag::calls % 100000 == 0)
    gpu_diag::report(gpu_sim_cycle + gpu_tot_sim_cycle);
  int clock_mask = next_clock_domain();''')
for old, new in [
    ('m_hbf_system->cycle_at(dram_time - m_config.dram_period);', '{ gpu_diag::Timer t(gpu_diag::hbf_bridge); m_hbf_system->cycle_at(dram_time - m_config.dram_period); }'),
    ('if (m_memory_partition_unit[partition]->hbf_cycle()) {\n          admitted = true;\n          last_admitted = partition;\n        }', '{ gpu_diag::Timer t(gpu_diag::hbf_partition);\n        if (m_memory_partition_unit[partition]->hbf_cycle()) {\n          admitted = true;\n          last_admitted = partition;\n        } }'),
    ('for (unsigned i = 0; i < m_memory_config->m_n_mem; i++) {\n      if (m_memory_config->simple_dram_model)', 'gpu_diag::Timer t(gpu_diag::dram_other);\n    for (unsigned i = 0; i < m_memory_config->m_n_mem; i++) {\n      if (m_memory_config->simple_dram_model)'),
    ('m_power_stats->pwr_mem_stat->l2_cache_stats[CURRENT_STAT_IDX].clear();', 'gpu_diag::Timer t(gpu_diag::l2);\n    m_power_stats->pwr_mem_stat->l2_cache_stats[CURRENT_STAT_IDX].clear();'),
    ('m_power_stats->pwr_mem_stat->core_cache_stats[CURRENT_STAT_IDX].clear();', 'gpu_diag::Timer t(gpu_diag::core);\n    m_power_stats->pwr_mem_stat->core_cache_stats[CURRENT_STAT_IDX].clear();'),
    ('    issue_block2core();\n    decrement_kernel_latency();', '    { gpu_diag::Timer t(gpu_diag::block_issue); issue_block2core(); }\n    decrement_kernel_latency();'),
]:
    assert s.count(old) >= 1, old
    s = s.replace(old, new, 1)
p.write_text(s)
print(p)
