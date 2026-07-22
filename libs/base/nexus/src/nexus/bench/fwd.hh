#pragma once

#include <clean-core/fwd.hh>

// Forward declarations and the logical counter vocabulary for the nx::bench benchmarking helpers.
//
// The first (and currently only) topic is hardware performance counters: cycles, instructions retired,
// branch mispredictions, cache misses, ... measured around a single invocation of a callable. Counters are
// addressed by a small platform-independent enum (nx::bench::hw_counter); the native, possibly cryptic name
// and a best-effort description travel alongside each one in the query API (see hardware_counters.hh).

namespace nx::bench
{
using namespace cc::primitive_defines;

/// A platform-independent hardware counter identity.
///
/// The values here are the portable default set. Not every CPU/OS can measure every one; what is actually
/// available right now is reported by available_hw_counters(). elapsed_nanoseconds and reference_cycles are
/// the always-on baseline that works even with no PMU access (wall clock + a cheap cycle counter).
enum class hw_counter : u8
{
    elapsed_nanoseconds,  ///< wall-clock time of the run (steady clock) — always available
    reference_cycles,     ///< reference cycles (x86 TSC / thread cycle time) — baseline, not the PMU cycle event
    instructions_retired, ///< retired instructions
    branch_instructions,  ///< retired branch instructions
    branch_misses,        ///< retired branches that were mispredicted
    cache_l1d_misses,     ///< L1 data-cache read misses
    cache_llc_references, ///< last-level-cache references
    cache_llc_misses,     ///< last-level-cache misses (miss here usually means a main-memory access)
};

struct hw_counter_info;
struct hw_counter_sample;
struct hw_measurement;
} // namespace nx::bench
