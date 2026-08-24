#pragma once

#include <clean-core/container/span.hh>
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
//
// cc::to_chars is the same rendering without the cc::string: it writes into a caller-provided char buffer and
// returns the length, so a stream writer can render straight into its window.
// =========================================================================================================

/// How a float is rendered: shortest round-trip, or one of the fixed-precision notations.
/// `shortest` ignores the precision argument and produces the shortest text that reads back as the same value.
enum class cc::float_notation : char
{
    shortest = 's',
    fixed = 'f',
    scientific = 'e',
    general = 'g',
};

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

// =========================================================================================================
// cc::to_chars — the same numbers, written into a caller-provided buffer.
//
// Returns the number of chars written; nothing is null-terminated.
// The buffer must be large enough (to_chars_int_max / to_chars_float_max always are) — a short one asserts.
// =========================================================================================================

/// Buffer sizes that fit any result of the corresponding overloads.
inline constexpr isize to_chars_int_max = 66;
inline constexpr isize to_chars_float_max = 512;

/// A precision below 0 means the notation's own default (6 digits for fixed / scientific / general).
[[nodiscard]] isize to_chars(span<char> out,
                             float v,
                             float_notation notation = float_notation::shortest,
                             isize precision = -1);
[[nodiscard]] isize to_chars(span<char> out,
                             double v,
                             float_notation notation = float_notation::shortest,
                             isize precision = -1);

// integer types, in base 10
// note: does not use the sized versions because this style is _complete_ for users
[[nodiscard]] isize to_chars(span<char> out, signed char v);
[[nodiscard]] isize to_chars(span<char> out, unsigned char v);
[[nodiscard]] isize to_chars(span<char> out, signed short v);
[[nodiscard]] isize to_chars(span<char> out, unsigned short v);
[[nodiscard]] isize to_chars(span<char> out, signed int v);
[[nodiscard]] isize to_chars(span<char> out, unsigned int v);
[[nodiscard]] isize to_chars(span<char> out, signed long v);
[[nodiscard]] isize to_chars(span<char> out, unsigned long v);
[[nodiscard]] isize to_chars(span<char> out, signed long long v);
[[nodiscard]] isize to_chars(span<char> out, unsigned long long v);

} // namespace cc
