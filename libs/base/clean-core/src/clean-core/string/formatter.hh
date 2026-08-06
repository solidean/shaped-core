#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

// =========================================================================================================
// cc::custom::formatter<T> — the customization point for making a type formattable with cc::format.
//
// Customization points live in the cc::custom namespace; specialize cc::custom::formatter<T> there to opt a
// type in.
// A specialization provides two static members:
//
//   static void format(cc::format_sink out, cc::string_view spec, T const& v);   // runtime: write v
//   static consteval void validate(cc::string_view spec);                        // compile-time: check spec
//
// The raw spec text (everything after ':') is passed through verbatim, so each type may define its own spec
// language.
// validate() is optional, and leaving it out opts the type out of compile-time spec checking entirely.
// Built-in types (arithmetic, char, bool, byte, pointers, strings) are formatted internally, but a
// cc::custom::formatter<T> is checked first and overrides even those.
//
// The delegation helpers cc::format_value and cc::validate_format_spec, a worked example, and how this
// tier ranks against the others are in libs/base/clean-core/docs/formatting.md.
// =========================================================================================================

namespace cc
{
/// A minimal, trivially-copyable output target for formatting: a context pointer plus a write function.
/// This is what a cc::custom::formatter writes into; call put() to append bytes.
/// No allocation is implied: the same sink backs both the allocating cc::format and the non-allocating
/// cc::format_to.
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
/// The primary template is intentionally incomplete: specialize it for your type to opt in.
/// The header comment above lists the required members, and libs/base/clean-core/docs/formatting.md walks
/// through a worked example.
template <class T>
struct formatter;
} // namespace custom
} // namespace cc
