#pragma once

#include <clean-core/string/string_view.hh>

namespace cc::rec
{
struct unit;
} // namespace cc::rec

// Performance reporting for PGO benchmarks; PGO_BENCHMARK in test.hh declares one.
// A PGO benchmark calls these to record named metrics onto the running test, and nexus collects them into a console
// table and the --pgo-json sidecar.
//
// Each call is a no-op outside a running test, so guarding is never required.
//
// **A metric is a value and a `cc::rec::unit`**, and the unit carries the orientation.
// That is deliberate: whether more is better is a property of the quantity rather than of the call site, and a
// benchmark that reported a latency as higher-is-better would make every speedup read backwards.
// nexus/bench/units.hh has the rate units; clean-core/record/stat.hh has the rest, and a caller defines its own next
// to the code that records it.
//
// docs/guides/perf-results.md is the workflow around them, including what `dev.py pgo` does with the sidecar.
// docs/guides/benchmarking.md is the other tool: a BENCHMARK measures, a PGO benchmark tracks.
namespace nx::pgo
{
/// Records `value` under `name`, in `unit`.
///
/// The unit decides how it prints and which direction is an improvement, so nothing else has to be passed.
void report(cc::string_view name, double value, cc::rec::unit const& unit);

/// Shorthand for a throughput in things per second (nx::bench::unit_items_per_second).
void report_elements_per_sec(cc::string_view name, double value);

/// Shorthand for a duration in seconds (cc::rec::unit_seconds), where less is better.
void report_time_for(cc::string_view name, double seconds);
} // namespace nx::pgo
