#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// cc::custom::formatter<T> — the customization point for making a type formattable with cc::format.
//
// Customization points live in the cc::custom namespace; specialize cc::custom::formatter<T> there to opt a
// type in. A specialization provides two static members:
//
//   static void format(cc::format_sink out, cc::string_view spec, T const& v);   // runtime: write v
//   static consteval void validate(cc::string_view spec);                        // compile-time: check spec
//
// The raw spec text (everything after ':') is passed through verbatim, so each type may define its own
// spec language. To reuse the standard grammar instead, delegate to cc::format_value (runtime) and
// cc::validate_format_spec (compile-time) from <clean-core/string/format.hh>.
//
// validate() is optional: if present it is called by the consteval format_string constructor (reach an
// error there — e.g. throw — to turn a bad spec into a compile error); if absent, no compile-time spec
// checking is performed for the type. Built-in types (arithmetic, char, bool, byte, pointers, strings) are
// handled internally and do not use this customization point.
// =========================================================================================================

namespace cc
{
/// A minimal, trivially-copyable output target for formatting: a context pointer plus a write function.
/// This is what a cc::custom::formatter writes into; call put() to append bytes. No allocation is implied —
/// the same sink backs both the allocating cc::format and the non-allocating cc::format_to.
struct format_sink
{
    void* ctx = nullptr;
    void (*write)(void* ctx, char const* data, isize size) = nullptr;

    /// Appends the bytes of s to the output.
    void put(string_view s) const { write(ctx, s.data(), s.size()); }

    /// Appends count copies of c to the output.
    void put_repeat(char c, isize count) const
    {
        for (isize i = 0; i < count; ++i)
            write(ctx, &c, 1);
    }
};

namespace custom
{
/// Customization point for formatting a type with cc::format.
///
/// The primary template is intentionally incomplete: specialize it for your type to opt in. See the header
/// comment for the required members.
///
/// Usage:
///   template <>
///   struct cc::custom::formatter<my_vec2>
///   {
///       static consteval void validate(cc::string_view spec)
///       {
///           if (!spec.empty())
///               throw "my_vec2 takes no format spec";
///       }
///       static void format(cc::format_sink out, cc::string_view /*spec*/, my_vec2 const& v)
///       {
///           out.put("(");
///           cc::format_value(out, "", v.x); // delegate the components to the standard formatting
///           out.put(", ");
///           cc::format_value(out, "", v.y);
///           out.put(")");
///       }
///   };
template <class T>
struct formatter;
} // namespace custom
} // namespace cc
