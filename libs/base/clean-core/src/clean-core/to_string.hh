#pragma once

#include <clean-core/string.hh>
#include <clean-core/string_view.hh>

// TODO
// - elaborate non-allocating design possible
// - how to handle locales? (or consistent story how and why not)
// - formatting config

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

// simply the char (NOT )
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

// no-op
[[nodiscard]] string to_string(char const* s);
[[nodiscard]] string to_string(string s);
[[nodiscard]] string to_string(string_view s);

} // namespace cc
