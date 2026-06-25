#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/format_spec.hh>
#include <clean-core/string/formatter.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>

// =========================================================================================================
// cc::format — a std::format / fmtlib-style formatter with Pythonic placeholders and compile-time-validated
// format strings.
//
//   cc::string s = cc::format("{} + {} = {}", 1, 2, 3);   // "1 + 2 = 3"
//   cc::format("{:#06x}", 255);                            // "0x00ff"
//   cc::format("{:>8.2f}", 3.14159);                       // "    3.14"
//
// The format string is checked at compile time: brace matching, argument indices, and spec-vs-argument-type
// compatibility are all verified by the consteval format_string constructor. A malformed call does not
// compile. Numeric formatting currently goes through std::to_chars behind the seam in format.cc.
//
// Customization: specialize cc::formatter<T> (spec-aware) or provide a member T::to_string() (plain "{}").
// See formatter.hh. No ADL is performed on arguments.
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
template <class... Args>
void format_dispatch(format_sink const& sink, string_view fmt, Args&&... args)
{
    format_arg_entry const entries[] = {
        format_arg_entry{.ptr = static_cast<void const*>(&args), .fn = &format_arg_thunk<std::remove_reference_t<Args>>}...,
        format_arg_entry{.ptr = nullptr, .fn = nullptr}, // sentinel: keeps the array non-empty when there are no args
    };
    render(sink, fmt, span<format_arg_entry const>(entries, isize(sizeof...(Args))));
}
} // namespace impl

template <class... Args>
template <class T>
    requires std::convertible_to<T const&, string_view>
consteval format_string<Args...>::format_string(T const& s) : _str(string_view(s))
{
    using namespace cc::impl;

    // one tag per argument (plus a sentinel so the array is never zero-sized)
    constexpr type_tag tags[] = {type_tag_of<Args>()..., type_tag::other_user};
    constexpr isize arg_count = isize(sizeof...(Args));

    isize const n = _str.size();
    isize pos = 0;
    index_state ix;

    while (pos < n)
    {
        char const c = _str[pos];
        if (c == '{')
        {
            if (pos + 1 < n && _str[pos + 1] == '{') // "{{" escape
            {
                pos += 2;
                continue;
            }

            field const f = parse_field(_str, pos, ix);
            if (f.arg_index < 0 || f.arg_index >= arg_count)
                format_error("argument index out of range");
            if (char const* err = spec_error_for_type(f.spec, tags[f.arg_index]))
                format_error(err);
            pos = f.next_pos;
        }
        else if (c == '}')
        {
            if (pos + 1 < n && _str[pos + 1] == '}') // "}}" escape
            {
                pos += 2;
                continue;
            }
            format_error("single '}' in format string (use '}}' for a literal brace)");
        }
        else
        {
            pos += 1;
        }
    }
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
