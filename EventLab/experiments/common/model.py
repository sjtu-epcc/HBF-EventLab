"""Fixed semantics of the supported OCP subset, not tunable experiment budgets.

Keep unsupported profiles explicit. Geometry and limits come from effective.ini.
These constants mirror common/types.hpp; changing the protocol needs a new contract.
"""
SPEC_PROFILE = "ocp_hbf_v0.7.0"
FRAGMENT_BYTES = 64
PAGE_BYTES = 4096
FRAGMENTS = PAGE_BYTES // FRAGMENT_BYTES
TICKS_PER_US = 1_000_000  # The simulator's fixed 1 ps Tick.
