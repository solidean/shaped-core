#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/formatter.hh>
#include <clean-core/string/impl/format_backend.hh>
#include <clean-core/string/impl/format_spec.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>
#include <utility>

// =========================================================================================================
// cc::format — a std::format / fmtlib-style formatter with Pythonic placeholders and compile-time-validated
// format strings.
//
//   cc::string s = cc::format("{} + {} = {}", 1, 2, 3);   // "1 + 2 = 3"
//   cc::format("{:#06x}", 255);                            // "0x00ff"
//   cc::format("{:>8.2f}", 3.14159);                       // "    3.14"
//   cc::format("{:'}", 1232453254);                        // "1'232'453'254"  (digit grouping)
//
// The format string is checked at compile time: brace matching, argument indices, and spec-vs-argument-type
// compatibility are all verified by the consteval format_string constructor. A malformed call does not
// compile. Numeric formatting currently goes through std::to_chars behind the seam in format.cc.
//
// Customization: specialize cc::custom::formatter<T> (it gets the raw spec string and owns its own runtime
// formatting + consteval validation) or provide a member T::to_string() (the plain "{}" spec). See
// formatter.hh. No ADL is performed on arguments.
// =========================================================================================================

namespace cc
{
//
// API
//

/// A format string whose syntax and argument types are validated at compile time.
///
/// Implicitly constructed from a string literal (or any compile-time string_view-convertible constant);
/// the consteval constructor parses it against Args... and turns any error into a compile error. You never
/// name this type directly — it is the first parameter of cc::format / format_append / format_to.
template <class... Args>
struct format_string
{
    template <class T>
        requires std::convertible_to<T const&, string_view>
    consteval format_string(T const& s);

    [[nodiscard]] constexpr string_view view() const { return _str; }

private:
    string_view _str;
};

/// Formats arguments into a freshly allocated string.
template <class... Args>
[[nodiscard]] string format(format_string<std::type_identity_t<Args>...> fmt, Args&&... args);

/// Appends formatted output to an existing string.
template <class... Args>
void format_append(string& out, format_string<std::type_identity_t<Args>...> fmt, Args&&... args);

/// Formats into a caller-provided buffer without allocating (snprintf semantics).
///
/// Writes at most out.size() bytes and returns the number of bytes that WOULD have been written; a return
/// value greater than out.size() means the output was truncated. The buffer is never null-terminated.
template <class... Args>
[[nodiscard]] isize format_to(span<char> out, format_string<std::type_identity_t<Args>...> fmt, Args&&... args);

//
// Implementation
//

namespace impl
{
/// Validates one field's spec text against argument type T at compile time: a cc::custom::formatter<T> owns
/// its validation (via an optional validate hook); otherwise the standard grammar + type rules apply.
template <class T>
consteval void validate_spec_for(string_view spec)
{
    using U = std::remove_cvref_t<T>;
    if constexpr (requires { sizeof(cc::custom::formatter<U>); }) // a cc::custom::formatter<U> exists
    {
        if constexpr (requires { cc::custom::formatter<U>::validate(spec); })
            cc::custom::formatter<U>::validate(spec);
        // else: the type opts out of compile-time spec validation; format() handles the spec at runtime
    }
    else
    {
        format_spec const parsed = parse_spec(spec); // syntax errors -> compile error
        if (char const* err = spec_error_for_type(parsed, type_tag_of<U>()))
            format_error(err);
    }
}

/// Walks the format string, replaying argument indexing, and validates the spec of every field that resolves
/// to argument `target` against type T. Structural errors are reported separately by validate_structure.
template <class T>
consteval void validate_arg_fields(string_view fmt, isize target)
{
    isize const n = fmt.size();
    isize pos = 0;
    index_state ix;

    while (pos < n)
    {
        char const c = fmt[pos];
        if (c == '{')
        {
            if (pos + 1 < n && fmt[pos + 1] == '{') // "{{" escape
            {
                pos += 2;
                continue;
            }
            field const f = parse_field(fmt, pos, ix);
            if (f.arg_index == target)
                validate_spec_for<T>(f.spec_text);
            pos = f.next_pos;
        }
        else if (c == '}')
        {
            if (pos + 1 < n && fmt[pos + 1] == '}') // "}}" escape
            {
                pos += 2;
                continue;
            }
            break; // structural error; already reported by validate_structure
        }
        else
        {
            pos += 1;
        }
    }
}
} // namespace impl

template <class... Args>
template <class T>
    requires std::convertible_to<T const&, string_view>
consteval format_string<Args...>::format_string(T const& s) : _str(string_view(s))
{
    // pass 1: structural validation (braces, escapes, index range, no auto/explicit mixing)
    cc::impl::validate_structure(_str, isize(sizeof...(Args)));

    // pass 2: validate each field's spec against the type of the argument it references
    [&]<std::size_t... Is>(std::index_sequence<Is...>)
    { (cc::impl::validate_arg_fields<Args>(_str, isize(Is)), ...); }(std::make_index_sequence<sizeof...(Args)>{});
}

template <class... Args>
[[nodiscard]] string format(format_string<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    string out = string::create_with_capacity(fmt.view().size() + 16);
    auto sink = cc::impl::make_string_sink(out);
    cc::impl::format_dispatch(sink, fmt.view(), cc::forward<Args>(args)...);
    return out;
}

template <class... Args>
void format_append(string& out, format_string<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    auto sink = cc::impl::make_string_sink(out);
    cc::impl::format_dispatch(sink, fmt.view(), cc::forward<Args>(args)...);
}

template <class... Args>
[[nodiscard]] isize format_to(span<char> out, format_string<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    cc::impl::span_sink_state state{.data = out.data(), .capacity = out.size(), .total = 0};
    auto sink = cc::impl::make_span_sink(state);
    cc::impl::format_dispatch(sink, fmt.view(), cc::forward<Args>(args)...);
    return state.total;
}
} // namespace cc
