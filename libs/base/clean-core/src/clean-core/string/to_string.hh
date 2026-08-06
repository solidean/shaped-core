#pragma once

#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// cc::to_string — one built-in value as a cc::string.
//
// An overload set over the built-in types only, so an unsupported type is a plain overload-resolution error.
// Numbers go straight through std::to_chars in to_string.cc and do not allocate.
// No locale handling anywhere on this path.
//
// Use cc::format to compose several values, and cc::to_debug_string for diagnostics.
// libs/base/clean-core/docs/formatting.md contrasts the three.
// =========================================================================================================

namespace cc
{
// TODO: move me
using byte = std::byte;

// in hex
[[nodiscard]] string to_string(void const* ptr);

// true/false
[[nodiscard]] string to_string(bool b);

// 0xFF
[[nodiscard]] string to_string(byte b);

// the char itself, unquoted — cc::to_debug_string is the one that quotes and escapes
[[nodiscard]] string to_string(char c);

// integer types
// note: does not use the sized versions because this style is _complete_ for users
[[nodiscard]] string to_string(signed char i);
[[nodiscard]] string to_string(unsigned char i);
[[nodiscard]] string to_string(signed short i);
[[nodiscard]] string to_string(unsigned short i);
[[nodiscard]] string to_string(signed int i);
[[nodiscard]] string to_string(unsigned int i);
[[nodiscard]] string to_string(signed long i);
[[nodiscard]] string to_string(unsigned long i);
[[nodiscard]] string to_string(signed long long i);
[[nodiscard]] string to_string(unsigned long long i);

// float/double
[[nodiscard]] string to_string(float i);
[[nodiscard]] string to_string(double i);

// copies into a cc::string
[[nodiscard]] string to_string(char const* s);
[[nodiscard]] string to_string(string s);
[[nodiscard]] string to_string(string_view s);

} // namespace cc
