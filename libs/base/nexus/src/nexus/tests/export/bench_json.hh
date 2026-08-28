#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/execute.hh>

namespace nx
{
/// Returns the benchmark sidecar for a completed execution.
///
/// **Everything, unrounded, including the full sample vector.**
/// The console report is a five-second read and throws most of this away on purpose; this is the other half of that
/// bargain, and it is what makes downstream tooling possible — a consumer holding the samples can recompute any
/// statistic this design got wrong, which no summary would allow.
///
/// Samples travel in the order they were taken rather than sorted, so drift across a run stays visible.
///
/// The system summary and the load brackets ride along, because a number without the machine is not interpretable and
/// a sidecar is exactly what gets read months later by someone who was not there.
///
/// `suite_name`, the program name, is echoed as "suite"; a test that measured nothing contributes nothing.
cc::string write_bench_json(cc::string_view suite_name, test_schedule_execution const& execution);
} // namespace nx
