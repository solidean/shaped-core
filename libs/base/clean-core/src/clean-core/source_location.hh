#pragma once

#include <source_location>

namespace cc
{
/// Type alias for std::source_location
/// Provides information about source code location (file, line, column, function)
/// Makes the clean-core stdlib more consistent while delegating to standard library
/// Usage:
///   void log(cc::source_location loc = cc::source_location::current()) {
///       std::cout << loc.file_name() << ":" << loc.line();
///   }
using source_location = std::source_location;
} // namespace cc
