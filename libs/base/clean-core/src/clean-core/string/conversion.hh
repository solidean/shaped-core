#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

namespace cc
{
/// Decodes UTF-8 bytes into UTF-16 code units.
/// BMP code points become one unit, astral ones a surrogate pair.
/// Malformed, overlong, or surrogate-encoding sequences each become U+FFFD.
/// The result is not NUL-terminated; append u'\0' yourself if a C-style wide string is needed.
[[nodiscard]] cc::vector<char16_t> utf8_to_utf16(cc::string_view utf8);

/// Encodes UTF-16 code units as UTF-8 bytes.
/// A well-formed surrogate pair becomes one astral code point; an unpaired surrogate becomes U+FFFD.
/// The input is a span rather than a view because a wide OS string is rarely NUL-terminated where its length is known.
[[nodiscard]] cc::string utf16_to_utf8(cc::span<char16_t const> utf16);
} // namespace cc
