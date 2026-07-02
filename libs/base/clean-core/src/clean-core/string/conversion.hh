#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

namespace cc
{
/// Decodes UTF-8 bytes into UTF-16 code units: BMP code points become one unit, astral ones a
/// surrogate pair. Malformed, overlong, or surrogate-encoding sequences each become U+FFFD.
/// The result is not NUL-terminated; append u'\0' yourself if a C-style wide string is needed.
[[nodiscard]] cc::vector<char16_t> utf8_to_utf16(cc::string_view utf8);
} // namespace cc
