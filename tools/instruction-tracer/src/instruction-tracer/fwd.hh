#pragma once

#include <clean-core/fwd.hh>

/// Forward declarations for instruction-tracer.
/// Each header defines its types qualified — `class itrace::trace_session { … };` — so the name has to be declared before the definition, and this is where it is.
namespace itrace
{
// Bare primitive names (`isize`, `u8`, `u32`, `u64`, …) inside `itrace`, matching the rest of the tree.
using namespace cc::primitive_defines;

// debug/
struct debug_config;
class debug_session;
class entry_breakpoint;
struct symbol_match;
struct source_line;
struct symbol_error;
class symbol_session;
struct trace_config;
class trace_session;

// report/
class source_cache;
} // namespace itrace
