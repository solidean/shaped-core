#pragma once

// =========================================================================================================
// cc::formatter<T> — the customization point for making a type formattable with cc::format.
//
// Specialize it for your type with a static
//     static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, T const& v);
// to make a custom type formattable (and spec-aware). Types with a member v.to_string() also work out of
// the box for the plain "{}" spec. No argument-dependent lookup is performed.
//
// The output sink, the value→text seam, the built-in specializations, and the dispatch all live in
// <clean-core/string/impl/format_backend.hh>.
// =========================================================================================================

namespace cc
{
/// Customization point for formatting a type with cc::format.
///
/// The primary template is intentionally incomplete: specialize it for your type to opt in. Built-in
/// specializations exist for the arithmetic types, char, bool, byte and raw pointers (string types are
/// handled directly by the dispatch, honoring width/precision).
///
/// Usage:
///   template <>
///   struct cc::formatter<my_vec2>
///   {
///       static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, my_vec2 const& v)
///       {
///           // e.g. delegate to the built-ins via cc::impl helpers, or write text directly
///       }
///   };
template <class T>
struct formatter;
} // namespace cc
