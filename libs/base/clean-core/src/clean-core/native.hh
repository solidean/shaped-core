#pragma once

#include <clean-core/fwd.hh>

// =========================================================================================================
// Platform-specific native utilities
// =========================================================================================================
//
// Symbol demangling:
//   demangle_symbol(symbol)     - demangle C++ symbol names to human-readable format
//

namespace cc
{
// =========================================================================================================
// Symbol demangling
// =========================================================================================================

/// Demangle a C++ mangled symbol name into a human-readable format.
/// Platform-specific implementation:
///   - MSVC: Uses UnDecorateSymbolName from dbghelp.dll
///   - GCC/Clang: Uses __cxa_demangle from libstdc++/libc++
///   - Other: Returns the input symbol unchanged
///
/// If demangling fails or is unavailable on the platform, returns the original symbol.
///
/// Usage:
///   auto mangled = "_Z3fooi";
///   auto demangled = cc::demangle_symbol(mangled);  // "foo(int)"
///
/// Complexity: O(symbol.size()) - depends on platform ABI implementation
[[nodiscard]] cc::string demangle_symbol(cc::string_view symbol);

} // namespace cc
