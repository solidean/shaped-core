#pragma once

#include <clean-core/fwd.hh>

namespace nx
{

// adopt clean-core's primitive vocabulary types (i8..i64, u8..u64, f32/f64, byte, isize)
// pulled into nx so they read as nx::i32 etc. without polluting the global namespace
using namespace cc::primitive_defines;

// Declared here so each header defines its types qualified rather than opening the namespace around them.
struct typed_value;          // the type-erased box an invocable test is called with (tests/typed_value.hh)
struct test_instance;        // one scheduled run of a test declaration (tests/schedule.hh)
struct test_schedule;        // the runs a CLI invocation selected (tests/schedule.hh)
struct test_schedule_config; // what the CLI arguments select (tests/schedule.hh)

} // namespace nx
