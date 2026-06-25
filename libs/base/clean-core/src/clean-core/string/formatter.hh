#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/char_predicates.hh>
#include <clean-core/string/format_spec.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>

// =========================================================================================================
// Per-type formatting: the cc::formatter<T> extension point, the output sink, the value→text seam, and
// the (no-ADL) dispatch used by the render loop.
//
// Extensibility: specialize cc::formatter<T> with a static
//     static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, T const& v);
// to make a custom type formattable (and spec-aware). Types with a member v.to_string() also work out of
// the box for the plain "{}" spec. No argument-dependent lookup is performed.
// =========================================================================================================

namespace cc
{
/// Customization point for formatting a type with cc::format.
///
/// The primary template is intentionally incomplete: specialize it for your type to opt in. Built-in
/// specializations exist for the arithmetic types, char, bool, byte and raw pointers (string types are
/// handled directly by the dispatch below, honoring width/precision).
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

namespace cc::impl
{
// -----------------------------------------------------------------------------------------------------
// Output sink — a POD, function-pointer-based byte sink (no virtuals), mirroring cc::memory_resource.
// One implementation appends to a cc::string; another writes into a fixed cc::span<char> with snprintf
// semantics (counting would-be bytes for truncation detection).
// -----------------------------------------------------------------------------------------------------

struct format_sink
{
    void* ctx = nullptr;
    void (*write)(void* ctx, char const* data, isize size) = nullptr;

    void put(string_view s) const { write(ctx, s.data(), s.size()); }
    void put_repeat(char c, isize count) const
    {
        for (isize i = 0; i < count; ++i)
            write(ctx, &c, 1);
    }
};

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
// Decoration (sign / prefix / fill / alignment / width / precision). Non-templated; defined in format.cc.
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

// -----------------------------------------------------------------------------------------------------
// Built-in cc::formatter specializations.
// -----------------------------------------------------------------------------------------------------

template <class T>
concept formattable_sint
    = std::is_integral_v<T> && std::is_signed_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>;
template <class T>
concept formattable_uint
    = std::is_integral_v<T> && !std::is_signed_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>;
template <class T>
concept formattable_ptr = std::is_pointer_v<T> && !std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>;

template <class>
inline constexpr bool always_false = false;
} // namespace cc::impl

namespace cc
{
template <class T>
    requires cc::impl::formattable_sint<T>
struct formatter<T>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, T v)
    {
        bool const neg = v < 0;
        u64 const mag = neg ? (u64(0) - u64(v)) : u64(v);
        cc::impl::format_integer(sink, spec, neg, mag);
    }
};

template <class T>
    requires cc::impl::formattable_uint<T>
struct formatter<T>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, T v)
    {
        cc::impl::format_integer(sink, spec, false, u64(v));
    }
};

template <class T>
    requires cc::impl::formattable_ptr<T>
struct formatter<T>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, T v)
    {
        char buf[cc::impl::chars_int_max];
        isize const n
            = cc::impl::chars_from_ptr(cc::span<char>(buf, cc::impl::chars_int_max), static_cast<void const*>(v));
        cc::impl::write_decorated_text(sink, spec, cc::string_view(buf, n), cc::impl::align_t::left);
    }
};

template <>
struct formatter<bool>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, bool v)
    {
        if (spec.presentation == '\0' || spec.presentation == 's')
            cc::impl::write_decorated_text(sink, spec, v ? cc::string_view("true") : cc::string_view("false"),
                                           cc::impl::align_t::left);
        else
            cc::impl::format_integer(sink, spec, false, v ? u64(1) : u64(0));
    }
};

template <>
struct formatter<char>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, char v)
    {
        if (spec.presentation == '\0' || spec.presentation == 'c')
            cc::impl::write_decorated_text(sink, spec, cc::string_view(&v, 1), cc::impl::align_t::left);
        else
        {
            int const iv = int(v);
            bool const neg = iv < 0;
            u64 const mag = neg ? (u64(0) - u64(iv)) : u64(iv);
            cc::impl::format_integer(sink, spec, neg, mag);
        }
    }
};

template <>
struct formatter<float>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, float v);
};

template <>
struct formatter<double>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, double v);
};

template <>
struct formatter<byte>
{
    static void format(cc::impl::format_sink const& sink, cc::impl::format_spec const& spec, byte v)
    {
        unsigned const b = static_cast<unsigned char>(v);
        if (spec.presentation == '\0') // default byte rendering matches cc::to_string(byte): "0x" + two uppercase hex digits
        {
            auto const hex = [](unsigned d) { return d < 10 ? char('0' + d) : char('A' + (d - 10)); };
            char const digits[2] = {hex((b >> 4) & 0xF), hex(b & 0xF)};
            cc::impl::write_decorated_number(sink, spec, false, cc::string_view("0x"), cc::string_view(digits, 2));
            return;
        }
        cc::impl::format_integer(sink, spec, false, u64(b));
    }
};
} // namespace cc

namespace cc::impl
{
/// Dispatches a single argument to its formatter (no ADL):
///   1. cc::formatter<U> specialization (built-ins + user types; spec-aware), else
///   2. anything convertible to string_view (honors width/precision), else
///   3. a member v.to_string() (plain "{}" only), else a compile error.
template <class T>
void format_one(format_sink const& sink, format_spec const& spec, T const& v)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (requires { cc::formatter<U>::format(sink, spec, v); })
    {
        cc::formatter<U>::format(sink, spec, v);
    }
    else if constexpr (std::is_convertible_v<T const&, string_view>)
    {
        write_decorated_text(sink, spec, string_view(v), align_t::left);
    }
    else if constexpr (requires {
                           { v.to_string() } -> std::convertible_to<string_view>;
                       })
    {
        auto const s = v.to_string();
        write_decorated_text(sink, spec, string_view(s), align_t::left);
    }
    else
    {
        static_assert(always_false<T>, "type is not formattable: specialize cc::formatter<T> or add a member "
                                       "to_string()");
    }
}

/// Type-erased argument entry used by the runtime render loop for positional access.
struct format_arg_entry
{
    void const* ptr = nullptr;
    void (*fn)(format_sink const& sink, format_spec const& spec, void const* ptr) = nullptr;
};

template <class T>
void format_arg_thunk(format_sink const& sink, format_spec const& spec, void const* ptr)
{
    format_one(sink, spec, *static_cast<T const*>(ptr));
}

/// Walks an already-validated format string, appending literals to the sink and dispatching each
/// replacement field to its argument. Non-templated (defined in format.cc) to keep template bloat low.
void render(format_sink const& sink, string_view fmt, span<format_arg_entry const> entries);
} // namespace cc::impl
