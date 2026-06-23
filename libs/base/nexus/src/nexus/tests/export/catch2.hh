#pragma once

#include <nexus/tests/execute.hh>
#include <nexus/tests/registry.hh>

#include <iosfwd>

namespace nx
{
/// Writes Catch2-compatible test discovery XML (a <MatchingTests> document) for
/// the registry to `out`. This is what C++ TestMate consumes to enumerate the
/// available tests of a binary.
void write_catch2_discovery_xml(std::ostream& out, test_registry const& registry);

/// Writes Catch2-compatible result XML (a <TestRun> document) for a completed
/// execution to `out`. Failed expressions are emitted per section; the number
/// of expressions per test case is capped to keep the output bounded.
void write_catch2_results_xml(std::ostream& out, test_schedule_execution const& execution);
} // namespace nx
