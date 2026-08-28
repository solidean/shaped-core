#pragma once

#include <clean-core/fwd.hh>
#include <nexus/fwd.hh>

// Forward declarations and the logical counter vocabulary for the nx::bench benchmarking helpers.
//
// Three topics live here.
// The measured run — run.hh, run_config.hh, result.hh — is the benchmarking harness proper: a callable, sampled and
// reported with statistics that say how much of the number to believe.
// The optimization barriers — barriers.hh — are what keep a measured body from being deleted.
// Hardware performance counters — hardware_counters.hh — are cycles, instructions retired, branch mispredictions and
// cache misses, measured around an invocation of a callable and addressed by the platform-independent hw_counter enum.
// The native — possibly cryptic — name and a best-effort description travel alongside each counter in
// hardware_counters.hh's query API.

namespace nx::bench
{
using namespace cc::primitive_defines;

// Declared ahead of the definitions below so that one can be written qualified.
enum class hw_counter : u8;
enum class warning_kind : u8;
enum class warning_severity : u8;

struct run_config;
struct report_style;
struct statistics;
struct warning;
struct recorded_quantity;
struct counter_reading;
struct result;
struct iteration;
struct calibration;
struct system_summary;
struct load_sample;

} // namespace nx::bench

/// A platform-independent hardware counter identity.
///
/// The values here are the portable default set, and not every CPU/OS can measure every one.
/// available_hw_counters() reports what is actually available right now.
/// elapsed_nanoseconds and reference_cycles are the always-on baseline — wall clock plus a cheap cycle counter — and work with no PMU access at all.
enum class nx::bench::hw_counter : nx::u8
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

namespace nx::bench
{

struct hw_counter_info;
struct hw_counter_sample;
struct hw_measurement;
} // namespace nx::bench
