#pragma once

#include <clean-core/error/optional.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// cc::from_string — one built-in value parsed back out of text.
//
// The inverse of cc::to_string, and deliberately no more than that: it accepts exactly what to_string
// produces and rejects everything else.
// So there is no leading '+', no surrounding whitespace, no thousands separator, no hex or octal prefix,
// and bool is "true"/"false" only.
// A caller wanting friendlier input — "yes", "0x1f", " 42 " — trims and dispatches itself.
//
// The whole view must be consumed: "12abc" and "12 " fail rather than yielding 12.
// A value the type cannot hold fails too, so 300 does not become 44 in a signed char.
//
// Two layers over one implementation.
// The out-parameter overload set is the primitive, complete over the built-ins, so an unsupported type is
// a plain overload-resolution error rather than a template error deep inside.
// cc::from_string<T>(sv) sits on top for the common "give me an optional" shape.
//
// Numbers go straight through std::from_chars in from_string.cc and never allocate.
// No locale handling anywhere on this path.
// =========================================================================================================

namespace cc
{
// true/false, exactly — case-sensitive
[[nodiscard]] bool from_string(string_view s, bool& out);

// integer types
// note: does not use the sized versions because this style is _complete_ for users
[[nodiscard]] bool from_string(string_view s, signed char& out);
[[nodiscard]] bool from_string(string_view s, unsigned char& out);
[[nodiscard]] bool from_string(string_view s, signed short& out);
[[nodiscard]] bool from_string(string_view s, unsigned short& out);
[[nodiscard]] bool from_string(string_view s, signed int& out);
[[nodiscard]] bool from_string(string_view s, unsigned int& out);
[[nodiscard]] bool from_string(string_view s, signed long& out);
[[nodiscard]] bool from_string(string_view s, unsigned long& out);
[[nodiscard]] bool from_string(string_view s, signed long long& out);
[[nodiscard]] bool from_string(string_view s, unsigned long long& out);

// float/double, including the "inf" and "nan" spellings std::to_chars produces
[[nodiscard]] bool from_string(string_view s, float& out);
[[nodiscard]] bool from_string(string_view s, double& out);

/// The same parse as the overload above, handed back as an optional.
/// `out` is untouched on failure in the overload form; here failure is simply cc::nullopt.
template <class T>
[[nodiscard]] optional<T> from_string(string_view s)
{
    T value = {};
    if (!cc::from_string(s, value))
        return cc::nullopt;
    return value;
}

} // namespace cc
