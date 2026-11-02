#pragma once
#include <stdint.h>
// Optional diagnostic ABI; target applications do not need to link HBFLab.
// Lookup HBFLabInjectionQuery through GetProcAddress on the injected DLL.
typedef struct HBFLabInjectionStats {
    uint32_t version;
    uint32_t active;
    uint64_t module_loads;
    uint64_t launches;
    uint64_t requests;
    uint64_t rejected;
    uint64_t live_modules;
} HBFLabInjectionStats;
typedef struct HBFLabInjectionAsyncStats {
    uint32_t version, active;
    uint64_t launches_completed, requests_completed, peak_active_launches;
} HBFLabInjectionAsyncStats;
