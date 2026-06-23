#pragma once

#include <nexus/tests/execute.hh>

#include <iosfwd>
#include <string_view>

namespace nx
{
/// Writes a JUnit XML report for a completed execution to `out`. Each nexus test
/// becomes one <testcase> under a single <testsuite> named `suite_name`; a
/// failing test carries a <failure> element listing its failed expressions and
/// the source location. The aggregate <testsuite>/<testsuites> attributes
/// (tests, failures, time) match what the dev.py tooling parses, so this output
/// is a drop-in for the synthesized sidecar.
void write_junit_xml(std::ostream& out, std::string_view suite_name, test_schedule_execution const& execution);
} // namespace nx
