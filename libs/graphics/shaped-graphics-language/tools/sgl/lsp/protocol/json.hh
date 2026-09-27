#pragma once

#include "fwd.hh"

#include <babel-data/data/json.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>

/// How an LSP type becomes JSON and back, uniformly, so a message struct only lists its fields.
///
/// Writing: a struct provides `write_fields(writer&, T const&)`, found by argument-dependent lookup, which writes its
/// members into an object the caller already opened; `put` does the opening, and handles scalars, strings, vectors
/// and optionals — an empty optional leaves its key out, which is what LSP means by an optional property.
///
/// Reading: a struct provides `read(babel::json::ref, T&) -> bool`, false when a required field is missing or has the
/// wrong kind, which the dispatcher answers with InvalidParams.

namespace lsp::json
{
using writer = babel::json::writer;
using ref = babel::json::ref;

} // namespace lsp::json

/// Already-serialized JSON, written as it is.
struct lsp::json::raw
{
    cc::string text;
};

/// JSON's `null`, for a result that is one.
struct lsp::json::null_t
{
};

namespace lsp::json
{

template <class T>
void put(writer& w, cc::string_view key, T const& value);
template <class T>
void put(writer& w, T const& value);

namespace impl
{
template <class T>
struct is_vector : std::false_type
{
};
template <class T>
struct is_vector<cc::vector<T>> : std::true_type
{
};
template <class T>
struct is_optional : std::false_type
{
};
template <class T>
struct is_optional<cc::optional<T>> : std::true_type
{
};
} // namespace impl

template <class T>
void put(writer& w, cc::string_view key, T const& value)
{
    if constexpr (std::is_same_v<T, raw>)
        w.write_raw(key, value.text);
    else if constexpr (std::is_same_v<T, null_t>)
        w.write(key, nullptr);
    else if constexpr (std::is_same_v<T, cc::string>)
        w.write(key, cc::string_view(value));
    else if constexpr (impl::is_optional<T>::value)
    {
        if (value.has_value())
            put(w, key, value.value());
    }
    else if constexpr (impl::is_vector<T>::value)
    {
        w.begin_array(key);
        for (auto const& e : value)
            put(w, e);
        w.end_array();
    }
    else if constexpr (babel::json::writable_scalar<T>)
        w.write(key, value);
    else
    {
        w.begin_object(key);
        write_fields(w, value);
        w.end_object();
    }
}

template <class T>
void put(writer& w, T const& value)
{
    if constexpr (std::is_same_v<T, raw>)
        w.write_raw(value.text);
    else if constexpr (std::is_same_v<T, null_t>)
        w.write(nullptr);
    else if constexpr (std::is_same_v<T, cc::string>)
        w.write(cc::string_view(value));
    else if constexpr (impl::is_optional<T>::value)
    {
        if (value.has_value())
            put(w, value.value());
        else
            w.write(nullptr);
    }
    else if constexpr (impl::is_vector<T>::value)
    {
        w.begin_array();
        for (auto const& e : value)
            put(w, e);
        w.end_array();
    }
    else if constexpr (babel::json::writable_scalar<T>)
        w.write(value);
    else
    {
        w.begin_object();
        write_fields(w, value);
        w.end_object();
    }
}

/// `value` as a JSON text of its own.
template <class T>
[[nodiscard]] cc::string to_text(T const& value)
{
    auto w = babel::json::string_writer();
    put(w.underlying(), value);
    auto text = w.finish();
    return text.has_value() ? cc::move(text.value()) : cc::string("null");
}

// reading scalars; each is false on a missing member, a wrong kind or a number that is no integer of the target type,
// and leaves `out` alone then

namespace impl
{
/// A number that is an exact integer within `I`'s range.
/// A fraction, NaN, an infinity or a value outside the range is false, since converting any of them is undefined.
template <class I>
[[nodiscard]] bool read_integral(ref in, I& out)
{
    static_assert(std::is_signed_v<I>, "the bounds assume a two's-complement signed type");
    if (!in.is_number())
        return false;
    auto const v = in.as_double();
    // the range is [-2^(n-1), 2^(n-1)), both exact as doubles; NaN fails the comparison
    auto const bound = double(u64(1) << (sizeof(I) * 8 - 1));
    if (!(v >= -bound && v < bound))
        return false;
    auto const i = I(v);
    if (double(i) != v)
        return false;
    out = i;
    return true;
}
} // namespace impl

[[nodiscard]] inline bool read(ref in, cc::string& out)
{
    if (!in.is_string())
        return false;
    out = cc::string(in.as_string());
    return true;
}

[[nodiscard]] inline bool read(ref in, i32& out)
{
    return impl::read_integral(in, out);
}

[[nodiscard]] inline bool read(ref in, i64& out)
{
    return impl::read_integral(in, out);
}

[[nodiscard]] inline bool read(ref in, bool& out)
{
    if (!in.is_bool())
        return false;
    out = in.as_bool();
    return true;
}

template <class T>
[[nodiscard]] bool read(ref in, cc::vector<T>& out)
{
    if (!in.is_array())
        return false;
    out.clear();
    for (auto i = isize(0); i < in.size(); ++i)
    {
        auto e = T();
        if (!read(in[i], e))
            return false;
        out.push_back(cc::move(e));
    }
    return true;
}

/// An optional member: absent or null is fine and leaves it empty, and a present one must read.
template <class T>
[[nodiscard]] bool read_optional(ref in, cc::optional<T>& out)
{
    if (!in.is_valid() || in.is_null())
    {
        out = cc::nullopt;
        return true;
    }
    auto v = T();
    if (!read(in, v))
        return false;
    out = cc::move(v);
    return true;
}
} // namespace lsp::json
