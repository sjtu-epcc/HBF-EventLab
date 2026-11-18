#ifndef HBF_EVENTLAB_BRIDGE_H
#define HBF_EVENTLAB_BRIDGE_H
#include <stdio.h>
class memory_config;
class gpgpu_sim;
class mem_fetch;

// C++11-safe boundary: EventLab's C++17 types remain private to the adapter.
class hbf_eventlab_bridge_t {
 public:
  hbf_eventlab_bridge_t(const memory_config *, gpgpu_sim *);
  ~hbf_eventlab_bridge_t();
  bool full() const;
  void push(mem_fetch *);
  void advance(double dram_time_seconds);
  mem_fetch *pop_return_for(unsigned source);
  bool busy() const;
  unsigned channels_per_stack() const;
  void print_stat(FILE *);
 private:
  struct implementation;
  implementation *state;
};
#endif
