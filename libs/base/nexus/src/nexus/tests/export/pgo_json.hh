#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/execute.hh>

namespace nx
{
/// Returns a perf-metrics JSON sidecar for a completed execution.
/// Every metric recorded via nx::pgo is emitted as one entry under a flat "metrics" array, tagged with its test name.
/// `suite_name`, the program name, is echoed as "suite", and a test that recorded no metrics contributes nothing.
/// The shape is a stable, machine-readable contract consumed by `dev.py pgo`; docs/guides/perf-results.md describes it.
cc::string write_pgo_json(cc::string_view suite_name, test_schedule_execution const& execution);
} // namespace nx
