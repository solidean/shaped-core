#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/execute.hh>

namespace nx
{
/// Returns a JUnit XML report for a completed execution. Each nexus test becomes
/// one <testcase> under a single <testsuite> named `suite_name`; a failing test
/// carries a <failure> element listing its failed expressions and the source
/// location. The aggregate <testsuite>/<testsuites> attributes (tests, failures,
/// time) match what the dev.py tooling parses, so this output is a drop-in for
/// the synthesized sidecar.
cc::string write_junit_xml(cc::string_view suite_name, test_schedule_execution const& execution);
} // namespace nx
