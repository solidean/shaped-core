#pragma once

#include <clean-core/string/string.hh>
#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>

namespace nx
{
/// Returns Catch2-compatible test discovery XML for the registry: a <MatchingTests> document.
/// This is what C++ TestMate consumes to enumerate a binary's available tests.
cc::string write_catch2_discovery_xml(test_registry const& registry);

/// Returns Catch2-compatible result XML for a completed execution: a <TestRun> document.
/// Failed expressions are emitted per section, and the count per test case is capped to keep the output bounded.
cc::string write_catch2_results_xml(test_schedule_execution const& execution);
} // namespace nx
