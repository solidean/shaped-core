#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/formatter.hh>
#include <clean-core/string/impl/format_spec.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>

// =========================================================================================================
// Rendering backend for cc::format (implementation detail of <clean-core/string/format.hh>).
//
// Holds the concrete sinks, the value→text seam (the only boundary touching std::to_chars, in format.cc),
// the decoration helpers, and the (no-ADL) per-argument dispatch + render plumbing. The built-in types are
// formatted here via cc::format_value; user types go through cc::custom::formatter<T> (formatter.hh). The
// public surface is cc::format_value / cc::validate_format_spec (delegation helpers) plus the cc::format /
// format_append / format_to functions (format.hh).
// =========================================================================================================

namespace cc::impl
{
// -----------------------------------------------------------------------------------------------------
// Concrete sinks backing cc::format_sink: one appends to a cc::string, one writes into a fixed
// cc::span<char> with snprintf semantics (counting would-be bytes for truncation detection).
// -----------------------------------------------------------------------------------------------------

format_sink make_string_sink(cc::string& out);

/// Mutable state for a span-backed sink. total counts bytes that WOULD be written (may exceed capacity).
struct span_sink_state
{
    char* data = nullptr;
    isize capacity = 0;
    isize total = 0;
};
format_sink make_span_sink(span_sink_state& state);

// -----------------------------------------------------------------------------------------------------
// Value → raw text seam. This is the ONLY boundary that touches std::to_chars (in format.cc); a vendored
// number-formatting backend can replace just these definitions. The functions emit raw digits/text (no
// sign, prefix, padding) — decoration is layered on top by write_decorated_*.
// -----------------------------------------------------------------------------------------------------

inline constexpr isize chars_int_max = 66;    // 64 binary digits + sign + prefix headroom
inline constexpr isize chars_float_max = 512; // ample for any double rendering

/// Writes the base-`base` digits of `v` (no sign, no prefix). Uppercases hex when `upper`. Returns count.
isize chars_from_u64(span<char> buf, u64 v, int base, bool upper);
/// Writes a float; mode is 's' (shortest round-trip), 'f', 'e', or 'g'. precision < 0 means shortest.
isize chars_from_f32(span<char> buf, float v, char mode, isize precision);
isize chars_from_f64(span<char> buf, double v, char mode, isize precision);
/// Writes "0x" followed by the hex address. Returns count.
isize chars_from_ptr(span<char> buf, void const* p);

// -----------------------------------------------------------------------------------------------------
// Decoration + per-type rendering (non-templated; defined in format.cc).
// -----------------------------------------------------------------------------------------------------

/// Writes `body` honoring width / alignment / fill, and (for strings) precision-as-max-length.
void write_decorated_text(format_sink const& sink, format_spec const& spec, string_view body, align_t default_align);
/// Writes sign + prefix + digits honoring width / alignment / fill, including '0'-padding between the
/// prefix and the digits when zero_pad is set and no explicit alignment was given.
void write_decorated_number(format_sink const& sink,
                            format_spec const& spec,
                            bool negative,
                            string_view prefix,
                            string_view digits);

/// Shared integer rendering: picks base/prefix from the spec and emits via write_decorated_number.
/// The 'c' presentation emits the value as a single character instead.
void format_integer(format_sink const& sink, format_spec const& spec, bool negative, u64 magnitude);
/// Float rendering (mode/precision/uppercase + optional grouping of the integer part).
void format_f32(format_sink const& sink, format_spec const& spec, float v);
void format_f64(format_sink const& sink, format_spec const& spec, double v);

// -----------------------------------------------------------------------------------------------------
// Built-in type classification.
// -----------------------------------------------------------------------------------------------------

template <class T>
concept formattable_sint
    = std::is_integral_v<T> && std::is_signed_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>;
template <class T>
concept formattable_uint
    = std::is_integral_v<T> && !std::is_signed_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>;
template <class T>
concept formattable_ptr = std::is_pointer_v<T> && !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>;

/// Types that cc::format_value can render with the standard grammar.
template <class T>
concept builtin_formattable = std::is_arithmetic_v<T> || std::is_same_v<T, byte> || formattable_ptr<T>
                           || std::is_convertible_v<T const&, cc::string_view>;

template <class>
inline constexpr bool always_false = false;
} // namespace cc::impl

namespace cc
{
/// Formats a built-in value (arithmetic, char, bool, byte, pointer, or anything string_view-convertible)
/// into `out` using the standard format spec grammar. This is the delegation target a cc::custom::formatter
/// can call to reuse the built-in formatting for its sub-values. `spec` is the raw spec text (may be empty).
template <class T>
void format_value(format_sink const& out, string_view spec, T const& v)
{
    using namespace cc::impl;
    using U = std::remove_cvref_t<T>;

    format_spec const s = parse_spec(spec);

    if constexpr (std::is_same_v<U, bool>)
    {
        if (s.presentation == '\0' || s.presentation == 's')
            write_decorated_text(out, s, v ? cc::string_view("true") : cc::string_view("false"), align_t::left);
        else
            format_integer(out, s, false, v ? u64(1) : u64(0));
    }
    else if constexpr (std::is_same_v<U, char>)
    {
        if (s.presentation == '\0' || s.presentation == 'c')
            write_decorated_text(out, s, cc::string_view(&v, 1), align_t::left);
        else
        {
            int const iv = int(v);
            bool const neg = iv < 0;
            format_integer(out, s, neg, neg ? (u64(0) - u64(iv)) : u64(iv));
        }
    }
    else if constexpr (std::is_same_v<U, byte>)
    {
        unsigned const b = static_cast<unsigned char>(v);
        if (s.presentation == '\0') // default byte rendering matches cc::to_string(byte): "0x" + two uppercase hex digits
        {
            auto const hex = [](unsigned d) { return d < 10 ? char('0' + d) : char('A' + (d - 10)); };
            char const digits[2] = {hex((b >> 4) & 0xF), hex(b & 0xF)};
            write_decorated_number(out, s, false, cc::string_view("0x"), cc::string_view(digits, 2));
        }
        else
            format_integer(out, s, false, u64(b));
    }
    else if constexpr (formattable_sint<U>)
    {
        bool const neg = v < 0;
        format_integer(out, s, neg, neg ? (u64(0) - u64(v)) : u64(v));
    }
    else if constexpr (formattable_uint<U>)
    {
        format_integer(out, s, false, u64(v));
    }
    else if constexpr (std::is_same_v<U, float>)
    {
        format_f32(out, s, v);
    }
    else if constexpr (std::is_same_v<U, double>)
    {
        format_f64(out, s, v);
    }
    else if constexpr (formattable_ptr<U>)
    {
        char buf[chars_int_max];
        isize const n = chars_from_ptr(cc::span<char>(buf, chars_int_max), static_cast<void const*>(v));
        write_decorated_text(out, s, cc::string_view(buf, n), align_t::left);
    }
    else if constexpr (std::is_convertible_v<U const&, cc::string_view>)
    {
        write_decorated_text(out, s, cc::string_view(v), align_t::left);
    }
    else
    {
        static_assert(cc::impl::always_false<T>, "cc::format_value: type is not a standard-formattable built-in");
    }
}

/// Validates that `spec` is a well-formed standard format spec (syntax only). Intended for delegation from
/// a cc::custom::formatter<T>::validate hook that accepts the standard grammar. Reaching an error during
/// constant evaluation turns a malformed spec into a compile error.
consteval void validate_format_spec(string_view spec)
{
    (void)cc::impl::parse_spec(spec);
}
} // namespace cc

namespace cc::impl
{
/// Dispatches a single argument to its formatter (no ADL):
///   1. a cc::custom::formatter<U> specialization (the user extension point), else
///   2. a built-in standard-formattable type (incl. string-likes) via cc::format_value, else
///   3. a member v.to_string() (the empty "{}" spec only), else a compile error.
template <class T>
void format_one(format_sink const& sink, string_view spec, T const& v)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (requires { sizeof(cc::custom::formatter<U>); }) // a cc::custom::formatter<U> specialization exists
    {
        static_assert(
            requires { cc::custom::formatter<U>::format(sink, spec, v); },
            "cc::custom::formatter<T> must define a static "
            "format(cc::format_sink, cc::string_view, T const&)");
        cc::custom::formatter<U>::format(sink, spec, v);
    }
    else if constexpr (builtin_formattable<U>)
    {
        cc::format_value(sink, spec, v);
    }
    else if constexpr (requires {
                           { v.to_string() } -> std::convertible_to<string_view>;
                       })
    {
        auto const s = v.to_string();
        cc::format_value(sink, spec, cc::string_view(s));
    }
    else
    {
        static_assert(always_false<T>, "type is not formattable: specialize cc::custom::formatter<T> or add a member "
                                       "to_string()");
    }
}

/// Type-erased argument entry used by the runtime render loop for positional access.
struct format_arg_entry
{
    void const* ptr = nullptr;
    void (*fn)(format_sink const& sink, string_view spec, void const* ptr) = nullptr;
};

template <class T>
void format_arg_thunk(format_sink const& sink, string_view spec, void const* ptr)
{
    format_one(sink, spec, *static_cast<T const*>(ptr));
}

/// Walks an already-validated format string, appending literals to the sink and dispatching each
/// replacement field to its argument. Non-templated (defined in format.cc) to keep template bloat low.
void render(format_sink const& sink, string_view fmt, span<format_arg_entry const> entries);

/// Builds the type-erased argument table and runs the render loop against the given sink. Shared by
/// cc::format / format_append / format_to.
template <class... Args>
void format_dispatch(format_sink const& sink, string_view fmt, Args&&... args)
{
    format_arg_entry const entries[] = {
        format_arg_entry{.ptr = static_cast<void const*>(&args), .fn = &format_arg_thunk<std::remove_reference_t<Args>>}...,
        format_arg_entry{.ptr = nullptr, .fn = nullptr}, // sentinel: keeps the array non-empty when there are no args
    };
    render(sink, fmt, span<format_arg_entry const>(entries, isize(sizeof...(Args))));
}
} // namespace cc::impl
