#pragma once

#include <stacktrace>

namespace cc
{
/// Type alias for std::stacktrace
/// Represents a snapshot of the program's call stack
/// Makes the clean-core stdlib more consistent while delegating to standard library
/// Usage:
///   cc::stacktrace trace = cc::stacktrace::current();
///   for (auto const& entry : trace) {
///       std::cout << entry.description() << '\n';
///   }
using stacktrace = std::stacktrace;

/// Type alias for std::stacktrace_entry
/// Represents a single frame in a stacktrace
using stacktrace_entry = std::stacktrace_entry;
} // namespace cc
