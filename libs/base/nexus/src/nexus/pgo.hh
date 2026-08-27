#pragma once

#include <clean-core/string/string_view.hh>

// Performance reporting for PGO benchmarks; PGO_BENCHMARK in test.hh declares one.
// A PGO benchmark calls these to record named metrics onto the running test, and nexus collects them into a console table and the --pgo-json sidecar.
//
// Each call is a no-op outside a running test, so guarding is never required.
// Metrics are free-form — name, value, unit — and the orientation (higher- vs lower-is-better) is what lets a reader or a tool compare runs correctly.
//
// docs/guides/perf-results.md is the workflow around them, including what `dev.py pgo` does with the sidecar.
namespace nx::pgo
{
// Records a throughput metric in elements per second (higher is better), e.g. hashed keys/s or allocations/s.
void report_elements_per_sec(cc::string_view name, double value);

// Records a duration metric in seconds (lower is better), e.g. time per operation.
void report_time_for(cc::string_view name, double seconds);

// Records an arbitrary metric with an explicit unit and orientation (e.g. "GB/s", "M ops/s").
void report_raw(cc::string_view name, double value, cc::string_view unit, bool higher_is_better = true);
} // namespace nx::pgo
