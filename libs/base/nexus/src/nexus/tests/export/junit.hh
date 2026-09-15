#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/execute.hh>

namespace nx
{
/// Returns a JUnit XML report for a completed execution.
/// Each nexus test becomes one <testcase> under a single <testsuite> named `suite_name`.
/// A failing test carries a <failure> element listing its failed expressions and their source locations.
/// The aggregate <testsuite> / <testsuites> attributes (tests, failures, time) match what the dev.py tooling parses, so this is a drop-in for the synthesized sidecar.
/// serial_time, serial_alone_time, serial_group and serial_group_time are test_schedule_execution::serial_time(), always present.
/// A measured `resources` adds cpu_load, cores_used and peak_resident_bytes beside them, which dev.py test's summary reads.
/// A `run_seed` adds a `seed` property to the suite, so a CI failure names the seed that reproduces it.
cc::string write_junit_xml(cc::string_view suite_name,
                           test_schedule_execution const& execution,
                           test_run_resources const& resources = {},
                           cc::optional<u64> run_seed = {});
} // namespace nx
