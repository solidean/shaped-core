#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <nexus/tests/execute.hh>

namespace nx
{
/// Returns a JSON sidecar placing every test of a completed execution on a wall-clock timeline.
/// One entry per test under a flat "tests" array, dispatched children included, named by their addressable path like the JUnit report.
/// "start" and "end" are Unix epoch seconds, converted from the steady clock with one offset taken here, so entries never overlap by clock jitter.
/// "thread" is the cc::current_thread_id() counter the body started on, and a test that never started is omitted.
/// `dev.py test --profile` consumes it to draw each test as its own slice of the run's trace.
cc::string write_timings_json(cc::string_view suite_name, test_schedule_execution const& execution);
} // namespace nx
