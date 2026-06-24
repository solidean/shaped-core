#pragma once

#include <clean-core/string/string.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>

namespace nx
{
/// Returns Catch2-compatible test discovery XML (a <MatchingTests> document) for
/// the registry. This is what C++ TestMate consumes to enumerate the available
/// tests of a binary.
cc::string write_catch2_discovery_xml(test_registry const& registry);

/// Returns Catch2-compatible result XML (a <TestRun> document) for a completed
/// execution. Failed expressions are emitted per section; the number of
/// expressions per test case is capped to keep the output bounded.
cc::string write_catch2_results_xml(test_schedule_execution const& execution);
} // namespace nx
